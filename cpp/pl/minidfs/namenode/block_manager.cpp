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

#include "cpp/pl/minidfs/namenode/block_manager.h"

#include <fmt/format.h>
#include <unordered_set>

#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/common/time_util.h"
#include "cpp/pl/minidfs/namenode/placement_manager.h"

namespace pl::minidfs {

BlockManager::BlockManager(MetadataStore* store, PlacementManager* placement)
    : store_(store), placement_(placement) {}

uint64_t BlockManager::next_generation_stamp() {
    return generation_stamp_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
}

pl::Result<LocatedBlock> BlockManager::allocate_block(uint64_t inode_id,
                                                      uint32_t block_index,
                                                      uint32_t replication) {
    // Allocate a block ID.
    auto id_result = store_->alloc_id("block");
    if (id_result.hasError()) {
        return folly::makeUnexpected(id_result.error());
    }

    uint64_t block_id = id_result.value();
    uint64_t gen = next_generation_stamp();
    uint64_t ts = now_ms();

    // Create block metadata entry (state = Allocating).
    BlockMeta block;
    block.block_id = block_id;
    block.inode_id = inode_id;
    block.block_index = block_index;
    block.generation_stamp = gen;
    block.length = 0;
    block.state = BlockState::kAllocating;
    block.desired_replica = replication;
    block.ctime_ms = ts;
    block.mtime_ms = ts;

    auto create_res = store_->create_block(block);
    if (create_res.hasError()) {
        return folly::makeUnexpected(create_res.error());
    }

    // Select datanodes for placement.
    auto targets = placement_->choose_targets(replication, std::nullopt);
    if (targets.hasError()) {
        return folly::makeUnexpected(targets.error());
    }

    // Register replicas (state = Writing).
    for (const auto& dn : targets.value()) {
        BlockReplica replica;
        replica.block_id = block_id;
        replica.datanode_id = dn.datanode_id;
        replica.storage_id = 0;
        replica.state = ReplicaState::kWriting;
        replica.length = 0;
        replica.generation_stamp = gen;
        replica.report_time_ms = ts;

        auto upsert_res = store_->upsert_replica(replica);
        if (upsert_res.hasError()) {
            return folly::makeUnexpected(upsert_res.error());
        }
    }

    // Build LocatedBlock response.
    LocatedBlock located;
    located.block_id = block_id;
    located.generation_stamp = gen;
    located.offset = 0; // caller will set actual offset
    located.length = 0;
    for (const auto& dn : targets.value()) {
        located.locations.push_back(DataNodeEndpoint{
            .datanode_id = dn.datanode_id,
            .host = dn.ip.empty() ? dn.hostname : dn.ip,
            .data_port = dn.data_port,
        });
    }
    return located;
}

pl::Result<pl::Void> BlockManager::commit_block(
    uint64_t block_id,
    uint64_t length,
    uint64_t generation_stamp,
    const std::vector<uint64_t>& finalized_datanode_ids) {
    if (finalized_datanode_ids.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInsufficientReplicas),
                             fmt::format("no finalized replicas reported for block {}", block_id));
    }

    auto block_result = store_->get_block(block_id);
    if (block_result.hasError()) {
        return folly::makeUnexpected(block_result.error());
    }
    auto& block = block_result.value();

    bool already_committed = block.state == BlockState::kCommitted && block.length == length &&
                             block.generation_stamp == generation_stamp;
    if (block.state != BlockState::kAllocating && !already_committed) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kBlockAlreadyCommitted),
                             fmt::format("block {} is already in state {}",
                                         block_id,
                                         static_cast<int>(block.state)));
    }

    std::unordered_set<uint64_t> finalized(finalized_datanode_ids.begin(),
                                           finalized_datanode_ids.end());

    auto replicas_result = store_->get_replicas(block_id);
    if (replicas_result.hasError()) {
        return folly::makeUnexpected(replicas_result.error());
    }
    uint32_t finalized_count = 0;
    for (const auto& r : replicas_result.value()) {
        if ((r.state == ReplicaState::kWriting || r.state == ReplicaState::kFinalized) &&
            finalized.contains(r.datanode_id)) {
            ++finalized_count;
        }
    }
    if (finalized_count == 0) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInsufficientReplicas),
                             fmt::format("no matching replicas finalized for block {}", block_id));
    }

    if (!already_committed) {
        block.state = BlockState::kCommitted;
        block.length = length;
        block.generation_stamp = generation_stamp;
        block.mtime_ms = now_ms();

        auto update_result = store_->update_block(block);
        if (update_result.hasError()) {
            return folly::makeUnexpected(update_result.error());
        }
    }

    // Transition only replicas that are known to have successfully written the block.
    for (const auto& r : replicas_result.value()) {
        if (r.state == ReplicaState::kWriting && finalized.contains(r.datanode_id)) {
            auto update_replica =
                store_->update_replica_state(r.block_id, r.datanode_id, ReplicaState::kFinalized);
            if (update_replica.hasError()) {
                return folly::makeUnexpected(update_replica.error());
            }
        }
    }

    return pl::Void{};
}

pl::Result<std::vector<LocatedBlock>> BlockManager::get_located_blocks(uint64_t inode_id) {
    auto blocks_result = store_->get_blocks_by_inode(inode_id);
    if (blocks_result.hasError()) {
        return folly::makeUnexpected(blocks_result.error());
    }

    std::vector<LocatedBlock> located_blocks;
    uint64_t offset = 0;

    for (const auto& block : blocks_result.value()) {
        if (block.state != BlockState::kCommitted) {
            continue; // Only return committed blocks for reading.
        }

        auto replicas_result = store_->get_replicas(block.block_id);
        if (replicas_result.hasError()) {
            return folly::makeUnexpected(replicas_result.error());
        }

        LocatedBlock lb;
        lb.block_id = block.block_id;
        lb.generation_stamp = block.generation_stamp;
        lb.offset = offset;
        lb.length = block.length;

        for (const auto& replica : replicas_result.value()) {
            if (replica.state != ReplicaState::kFinalized) {
                continue;
            }
            auto dn_result = store_->get_datanode(replica.datanode_id);
            if (dn_result.hasError()) {
                continue; // Skip unavailable datanodes.
            }
            lb.locations.push_back(DataNodeEndpoint{
                .datanode_id = dn_result.value().datanode_id,
                .host = dn_result.value().ip.empty() ? dn_result.value().hostname
                                                     : dn_result.value().ip,
                .data_port = dn_result.value().data_port,
            });
        }

        if (!lb.locations.empty()) {
            located_blocks.push_back(std::move(lb));
        }
        offset += block.length;
    }
    return located_blocks;
}

pl::Result<pl::Void> BlockManager::report_corrupt_replica(uint64_t block_id, uint64_t datanode_id) {
    return store_->update_replica_state(block_id, datanode_id, ReplicaState::kCorrupt);
}

pl::Result<pl::Void> BlockManager::invalidate_blocks(uint64_t inode_id) {
    auto blocks_result = store_->get_blocks_by_inode(inode_id);
    if (blocks_result.hasError()) {
        return folly::makeUnexpected(blocks_result.error());
    }

    for (auto& block : blocks_result.value()) {
        auto replicas = store_->get_replicas(block.block_id);
        if (replicas.hasError()) {
            return folly::makeUnexpected(replicas.error());
        }
        for (const auto& replica : replicas.value()) {
            if (replica.state != ReplicaState::kDeleted) {
                auto mark = store_->update_replica_state(
                    block.block_id, replica.datanode_id, ReplicaState::kDeleting);
                if (mark.hasError()) {
                    return folly::makeUnexpected(mark.error());
                }
            }
        }
        // Mark block as deleted.
        block.state = BlockState::kDeleted;
        block.mtime_ms = now_ms();
        auto upd = store_->update_block(block);
        if (upd.hasError()) {
            return folly::makeUnexpected(upd.error());
        }
    }
    return pl::Void{};
}

pl::Result<std::vector<BlockMeta>> BlockManager::get_blocks_to_delete(uint64_t datanode_id) {
    auto replicas = store_->get_replicas_by_datanode(datanode_id);
    if (replicas.hasError()) {
        return folly::makeUnexpected(replicas.error());
    }

    std::vector<BlockMeta> blocks;
    for (const auto& replica : replicas.value()) {
        if (replica.state != ReplicaState::kDeleting) {
            continue;
        }
        auto block = store_->get_block(replica.block_id);
        if (block.hasError()) {
            continue;
        }
        blocks.push_back(block.value());
    }
    return blocks;
}

pl::Result<pl::Void> BlockManager::reconcile_block_report(
    uint64_t datanode_id, const std::unordered_set<uint64_t>& reported_block_ids) {
    auto replicas = store_->get_replicas_by_datanode(datanode_id);
    if (replicas.hasError()) {
        return folly::makeUnexpected(replicas.error());
    }

    for (const auto& replica : replicas.value()) {
        if (replica.state == ReplicaState::kDeleting &&
            !reported_block_ids.contains(replica.block_id)) {
            auto mark =
                store_->update_replica_state(replica.block_id, datanode_id, ReplicaState::kDeleted);
            if (mark.hasError()) {
                return folly::makeUnexpected(mark.error());
            }
        }
    }
    return pl::Void{};
}

} // namespace pl::minidfs
