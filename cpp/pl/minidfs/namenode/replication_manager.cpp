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

#include "cpp/pl/minidfs/common/constants.h"

namespace pl::minidfs {

ReplicationManager::ReplicationManager(MetadataStore* store, PlacementManager* placement)
    : store_(store), placement_(placement) {}

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

        // Count healthy (finalized) replicas.
        auto& replicas = replicas_result.value();
        uint32_t healthy_count = 0;
        uint64_t source_dn = 0;
        std::vector<uint64_t> existing_dns;

        for (const auto& r : replicas) {
            if (r.state == ReplicaState::kFinalized) {
                ++healthy_count;
                source_dn = r.datanode_id;
                existing_dns.push_back(r.datanode_id);
            }
        }

        if (healthy_count < block.desired_replica && healthy_count > 0) {
            // Under-replicated: need to add replicas.
            uint32_t deficit = block.desired_replica - healthy_count;
            auto targets = placement_->choose_targets(deficit, std::nullopt);
            if (targets.hasError()) {
                continue; // Cannot find targets, skip.
            }

            for (const auto& target : targets.value()) {
                // Don't replicate to a node that already has this block.
                bool already_has =
                    std::find(existing_dns.begin(), existing_dns.end(), target.datanode_id) !=
                    existing_dns.end();
                if (already_has) {
                    continue;
                }

                ReplicationTask task;
                task.block_id = block.block_id;
                task.source_datanode = source_dn;
                task.target_datanode = target.datanode_id;
                task.is_deletion = false;
                tasks.push_back(task);
                ++task_count;
            }
        } else if (healthy_count > block.desired_replica) {
            // Over-replicated: need to remove excess replicas.
            uint32_t excess = healthy_count - block.desired_replica;
            // Remove replicas from nodes with least free space (greedy).
            // For simplicity, just pick the last N in the replica list.
            uint32_t removed = 0;
            for (auto it = replicas.rbegin(); it != replicas.rend() && removed < excess; ++it) {
                if (it->state == ReplicaState::kFinalized) {
                    ReplicationTask task;
                    task.block_id = block.block_id;
                    task.source_datanode = 0;
                    task.target_datanode = it->datanode_id;
                    task.is_deletion = true;
                    tasks.push_back(task);
                    ++task_count;
                    ++removed;
                }
            }
        }
    }

    return tasks;
}

} // namespace pl::minidfs
