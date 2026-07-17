// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/05/10 17:30

#include "cpp/pl/minidfs/namenode/replication_manager.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "cpp/pl/minidfs/common/block_token.h"
#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/time_util.h"

namespace pl::minidfs {

namespace {

protocol::BlockTokenProto make_replication_token(uint64_t block_id,
                                                 uint64_t generation_stamp,
                                                 uint64_t inode_id,
                                                 uint32_t block_index,
                                                 const std::string& secret) {
    return issue_block_token(block_id,
                             generation_stamp,
                             inode_id,
                             block_index,
                             kBlockTokenPermissionTransfer,
                             default_block_token_ttl_ms(),
                             secret);
}

BlockToken from_proto_token(const protocol::BlockTokenProto& token) {
    return BlockToken{
        .block_id = token.block_id(),
        .generation_stamp = token.generation_stamp(),
        .inode_id = token.inode_id(),
        .block_index = token.block_index(),
        .permissions = token.permissions(),
        .expires_at_ms = token.expires_at_ms(),
        .signature = token.signature(),
    };
}

} // namespace

ReplicationManager::ReplicationManager(MetadataStore* store,
                                       PlacementManager* placement,
                                       std::string block_token_secret)
    : store_(store), placement_(placement), block_token_secret_(std::move(block_token_secret)) {}

pl::Result<std::vector<ReplicationTask>> ReplicationManager::scan() {
    // Get all committed blocks.
    auto blocks_result = store_->get_blocks_by_state(BlockState::kCommitted);
    if (blocks_result.hasError()) {
        return folly::makeUnexpected(blocks_result.error());
    }

    std::vector<ReplicationTask> tasks;
    uint32_t task_count = 0;

    for (const auto& block : blocks_result.value()) {
        if (task_count >= kDefaultMaxReplicationTasksPerRound) {
            break;
        }

        auto replicas_result = store_->get_replicas(block.block_id);
        if (replicas_result.hasError()) {
            continue; // Skip blocks we can't query.
        }

        // Count only finalized replicas on live datanodes as healthy. Existing
        // writing replicas are durable pending work and must not be scheduled
        // again onto another target.
        auto& replicas = replicas_result.value();
        std::vector<uint64_t> healthy_sources;
        std::vector<uint64_t> pending_targets;
        std::unordered_set<uint64_t> existing_dns;
        std::unordered_set<uint64_t> healthy_dns;

        for (const auto& r : replicas) {
            existing_dns.insert(r.datanode_id);
            auto dn = store_->get_datanode(r.datanode_id);
            bool is_live = dn.hasValue() && dn.value().state == DataNodeState::kLive;
            if (r.state == ReplicaState::kFinalized && is_live) {
                healthy_sources.push_back(r.datanode_id);
                healthy_dns.insert(r.datanode_id);
            } else if (r.state == ReplicaState::kWriting && is_live) {
                pending_targets.push_back(r.datanode_id);
            }
        }

        uint32_t healthy_count = healthy_sources.size();
        uint32_t effective_count = healthy_count + pending_targets.size();
        if (effective_count < block.desired_replica && !healthy_sources.empty()) {
            uint32_t deficit = block.desired_replica - effective_count;
            for (uint32_t i = 0; i < deficit; ++i) {
                auto targets = placement_->choose_targets(1, existing_dns);
                if (targets.hasError()) {
                    break;
                }
                const auto& target = targets.value().front();
                BlockReplica replica;
                replica.block_id = block.block_id;
                replica.datanode_id = target.datanode_id;
                replica.state = ReplicaState::kWriting;
                replica.length = block.length;
                replica.generation_stamp = block.generation_stamp;
                replica.report_time_ms = now_ms();
                auto upsert = store_->upsert_replica(replica);
                if (upsert.hasError()) {
                    return folly::makeUnexpected(upsert.error());
                }
                existing_dns.insert(target.datanode_id);
                pending_targets.push_back(target.datanode_id);
            }
        } else if (healthy_count > block.desired_replica) {
            // Over-replicated: need to remove excess replicas.
            uint32_t excess = healthy_count - block.desired_replica;
            // Remove replicas from nodes with least free space (greedy).
            // For simplicity, just pick the last N in the replica list.
            uint32_t removed = 0;
            for (auto it = replicas.rbegin(); it != replicas.rend() && removed < excess &&
                                              task_count < kDefaultMaxReplicationTasksPerRound;
                 ++it) {
                if (it->state == ReplicaState::kFinalized &&
                    healthy_dns.contains(it->datanode_id)) {
                    auto mark = store_->update_replica_state(
                        block.block_id, it->datanode_id, ReplicaState::kDeleting);
                    if (mark.hasError()) {
                        return folly::makeUnexpected(mark.error());
                    }
                    ReplicationTask task;
                    task.block_id = block.block_id;
                    task.source_datanode = 0;
                    task.target_datanode = it->datanode_id;
                    task.inode_id = block.inode_id;
                    task.block_index = block.block_index;
                    task.generation_stamp = block.generation_stamp;
                    task.is_deletion = true;
                    tasks.push_back(task);
                    ++task_count;
                    ++removed;
                }
            }
        }

        if (healthy_sources.empty()) {
            continue;
        }
        for (uint64_t target : pending_targets) {
            if (task_count >= kDefaultMaxReplicationTasksPerRound) {
                break;
            }
            auto token = make_replication_token(block.block_id,
                                                block.generation_stamp,
                                                block.inode_id,
                                                block.block_index,
                                                block_token_secret_);
            tasks.push_back(ReplicationTask{
                .block_id = block.block_id,
                .source_datanode = healthy_sources.front(),
                .target_datanode = target,
                .inode_id = block.inode_id,
                .block_index = block.block_index,
                .generation_stamp = block.generation_stamp,
                .block_token = from_proto_token(token),
                .is_deletion = false,
            });
            ++task_count;
        }
    }

    return tasks;
}

} // namespace pl::minidfs
