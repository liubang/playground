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
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/common/time_util.h"
#include "cpp/pl/minidfs/namenode/placement_manager.h"

namespace pl::minidfs {

BlockManager::BlockManager(MetadataStore* store,
                           PlacementManager* placement,
                           std::string token_secret)
    : store_(store), placement_(placement), token_secret_(std::move(token_secret)) {}

protocol::BlockTokenProto BlockManager::issue_block_token(uint64_t block_id,
                                                           uint64_t generation_stamp,
                                                           uint64_t inode_id,
                                                           uint32_t block_index,
                                                           uint32_t permissions) const {
    return pl::minidfs::issue_block_token(block_id,
                                          generation_stamp,
                                          inode_id,
                                          block_index,
                                          permissions,
                                          default_block_token_ttl_ms(),
                                          token_secret_);
}

pl::Result<uint64_t> BlockManager::next_generation_stamp() {
    return store_->alloc_id("generation_stamp");
}

pl::Result<LocatedBlock> BlockManager::allocate_block(uint64_t inode_id,
                                                      uint32_t block_index,
                                                      uint32_t replication) {
    auto txn = store_->begin_transaction();
    if (txn.hasError()) {
        return folly::makeUnexpected(txn.error());
    }

    auto inode = store_->get_inode(inode_id);
    if (inode.hasError()) {
        return folly::makeUnexpected(inode.error());
    }
    if (inode.value().type != InodeType::kFile ||
        inode.value().state != FileState::kUnderConstruction) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kFileUnderConstruction),
                             "blocks can only be allocated for files under construction");
    }

    auto existing_blocks = store_->get_blocks_by_inode(inode_id);
    if (existing_blocks.hasError()) {
        return folly::makeUnexpected(existing_blocks.error());
    }
    for (const auto& block : existing_blocks.value()) {
        if (block.block_index == block_index && block.state != BlockState::kDeleted) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kAlreadyExists),
                                 fmt::format("block index {} already exists", block_index));
        }
    }

    auto id_result = store_->alloc_id("block");
    if (id_result.hasError()) {
        return folly::makeUnexpected(id_result.error());
    }

    auto generation_stamp = next_generation_stamp();
    if (generation_stamp.hasError()) {
        return folly::makeUnexpected(generation_stamp.error());
    }

    uint64_t block_id = id_result.value();
    uint64_t gen = generation_stamp.value();
    uint64_t ts = now_ms();
    bool block_created = false;

    auto cleanup_ghost_block = [&]() -> pl::Result<pl::Void> {
        auto del_replicas = store_->delete_replicas_by_block(block_id);
        if (del_replicas.hasError()) {
            return folly::makeUnexpected(del_replicas.error());
        }
        auto del_block = store_->delete_block(block_id);
        if (del_block.hasError()) {
            return folly::makeUnexpected(del_block.error());
        }
        return pl::Void{};
    };

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
        const auto code = create_res.error().code();
        const auto msg = std::string(create_res.error().message());
        const bool duplicate =
            code == static_cast<pl::status_code_t>(ErrorCode::kAlreadyExists) ||
            (code == static_cast<pl::status_code_t>(ErrorCode::kMySQLQueryFailed) &&
             msg.find("Duplicate entry") != std::string::npos);
        if (duplicate) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kAlreadyExists),
                                 fmt::format("block index {} already exists", block_index));
        }
        return folly::makeUnexpected(create_res.error());
    }
    block_created = true;

    auto targets = placement_->choose_targets(replication, std::nullopt);
    if (targets.hasError()) {
        if (block_created) {
            (void)cleanup_ghost_block();
        }
        return folly::makeUnexpected(targets.error());
    }

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
            if (block_created) {
                (void)cleanup_ghost_block();
            }
            return folly::makeUnexpected(upsert_res.error());
        }
    }

    auto commit = txn.value()->commit();
    if (commit.hasError()) {
        if (block_created) {
            (void)cleanup_ghost_block();
        }
        return folly::makeUnexpected(commit.error());
    }

    LocatedBlock located;
    located.block_id = block_id;
    located.generation_stamp = gen;
    located.offset = 0;
    located.length = 0;
    located.block_token = {
        .block_id = block_id,
        .generation_stamp = gen,
        .inode_id = inode_id,
        .block_index = block_index,
        .permissions = all_data_plane_permissions(),
        .expires_at_ms = 0,
        .signature = "",
    };
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
    uint32_t required_finalized = std::min<uint32_t>(kMinWriteReplica, block.desired_replica);
    if (finalized_count < required_finalized) {
        return pl::makeError(
            static_cast<pl::status_code_t>(ErrorCode::kInsufficientReplicas),
            fmt::format("block {} requires {} finalized replicas, only {} matched",
                        block_id,
                        required_finalized,
                        finalized_count));
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
            auto token = issue_block_token(block.block_id,
                                           block.generation_stamp,
                                           block.inode_id,
                                           block.block_index,
                                           kBlockTokenPermissionRead);
            lb.block_token = {
                .block_id = token.block_id(),
                .generation_stamp = token.generation_stamp(),
                .inode_id = token.inode_id(),
                .block_index = token.block_index(),
                .permissions = token.permissions(),
                .expires_at_ms = token.expires_at_ms(),
                .signature = token.signature(),
            };
            located_blocks.push_back(std::move(lb));
        }
        offset += block.length;
    }
    return located_blocks;
}

pl::Result<pl::Void> BlockManager::report_corrupt_replica(uint64_t block_id, uint64_t datanode_id) {
    return store_->update_replica_state(block_id, datanode_id, ReplicaState::kCorrupt);
}

pl::Result<pl::Void> BlockManager::invalidate_block(BlockMeta* block) {
    auto replicas = store_->get_replicas(block->block_id);
    if (replicas.hasError()) {
        return folly::makeUnexpected(replicas.error());
    }
    for (const auto& replica : replicas.value()) {
        if (replica.state == ReplicaState::kDeleted) {
            continue;
        }
        auto mark = store_->update_replica_state(
            block->block_id, replica.datanode_id, ReplicaState::kDeleting);
        if (mark.hasError()) {
            return folly::makeUnexpected(mark.error());
        }
    }
    block->state = BlockState::kDeleted;
    block->mtime_ms = now_ms();
    return store_->update_block(*block);
}

pl::Result<pl::Void> BlockManager::invalidate_blocks(uint64_t inode_id) {
    auto blocks_result = store_->get_blocks_by_inode(inode_id);
    if (blocks_result.hasError()) {
        return folly::makeUnexpected(blocks_result.error());
    }

    for (auto& block : blocks_result.value()) {
        if (block.state != BlockState::kDeleted) {
            auto invalidate = invalidate_block(&block);
            if (invalidate.hasError()) {
                return invalidate;
            }
        }
    }
    return pl::Void{};
}

pl::Result<uint64_t> BlockManager::recover_file(uint64_t inode_id) {
    auto blocks_result = store_->get_blocks_by_inode(inode_id);
    if (blocks_result.hasError()) {
        return folly::makeUnexpected(blocks_result.error());
    }

    uint64_t final_length = 0;
    for (auto& block : blocks_result.value()) {
        if (block.state == BlockState::kCommitted) {
            final_length += block.length;
            continue;
        }
        if (block.state == BlockState::kDeleted) {
            continue;
        }

        auto invalidate = invalidate_block(&block);
        if (invalidate.hasError()) {
            return folly::makeUnexpected(invalidate.error());
        }
    }
    return final_length;
}

pl::Result<pl::Void> BlockManager::truncate_file(uint64_t inode_id,
                                                 uint64_t new_length,
                                                 const TruncateReplicaFunc& truncate_replica) {
    auto inode = store_->get_inode(inode_id);
    if (inode.hasError()) {
        return folly::makeUnexpected(inode.error());
    }
    if (inode.value().type != InodeType::kFile || inode.value().state != FileState::kNormal) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kFileUnderConstruction),
                             "only closed files can be truncated");
    }
    if (new_length > inode.value().length) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                             "truncate length exceeds file length");
    }
    if (new_length == inode.value().length) {
        return pl::Void{};
    }

    auto blocks = store_->get_blocks_by_inode(inode_id);
    if (blocks.hasError()) {
        return folly::makeUnexpected(blocks.error());
    }

    uint64_t offset = 0;
    for (auto& block : blocks.value()) {
        if (block.state == BlockState::kDeleted) {
            continue;
        }
        if (block.state != BlockState::kCommitted) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                                 "closed file contains an uncommitted block");
        }

        uint64_t block_end = offset + block.length;
        if (offset >= new_length) {
            auto invalidate = invalidate_block(&block);
            if (invalidate.hasError()) {
                return invalidate;
            }
        } else if (block_end > new_length) {
            uint64_t truncated_length = new_length - offset;
            auto replicas = store_->get_replicas(block.block_id);
            if (replicas.hasError()) {
                return folly::makeUnexpected(replicas.error());
            }

            uint32_t successful_replicas = 0;
            for (auto replica : replicas.value()) {
                if (replica.state != ReplicaState::kFinalized) {
                    continue;
                }
                auto truncate = truncate_replica(replica, truncated_length);
                if (truncate.hasError()) {
                    auto deleting = store_->update_replica_state(
                        block.block_id, replica.datanode_id, ReplicaState::kDeleting);
                    if (deleting.hasError()) {
                        return folly::makeUnexpected(deleting.error());
                    }
                    continue;
                }
                replica.length = truncated_length;
                replica.report_time_ms = now_ms();
                auto update = store_->upsert_replica(replica);
                if (update.hasError()) {
                    return folly::makeUnexpected(update.error());
                }
                ++successful_replicas;
            }
            if (successful_replicas == 0) {
                return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kIOError),
                                     "failed to truncate every finalized block replica");
            }

            block.length = truncated_length;
            block.mtime_ms = now_ms();
            auto update = store_->update_block(block);
            if (update.hasError()) {
                return folly::makeUnexpected(update.error());
            }
        }
        offset = block_end;
    }
    return pl::Void{};
}

pl::Result<pl::Void> BlockManager::set_replication(uint64_t inode_id, uint32_t replication) {
    auto blocks = store_->get_blocks_by_inode(inode_id);
    if (blocks.hasError()) {
        return folly::makeUnexpected(blocks.error());
    }
    for (auto& block : blocks.value()) {
        if (block.state == BlockState::kDeleted) {
            continue;
        }
        block.desired_replica = replication;
        block.mtime_ms = now_ms();
        auto update = store_->update_block(block);
        if (update.hasError()) {
            return folly::makeUnexpected(update.error());
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
    uint64_t datanode_id, const std::vector<ReportedBlock>& reported_blocks, bool full_report) {
    auto replicas = store_->get_replicas_by_datanode(datanode_id);
    if (replicas.hasError()) {
        return folly::makeUnexpected(replicas.error());
    }

    std::unordered_map<uint64_t, ReportedBlock> reported;
    reported.reserve(reported_blocks.size());
    for (const auto& block : reported_blocks) {
        reported.emplace(block.block_id, block);
    }

    for (const auto& replica : replicas.value()) {
        auto report = reported.find(replica.block_id);
        bool is_reported = report != reported.end();
        if (replica.state == ReplicaState::kDeleting && full_report && !is_reported) {
            auto mark =
                store_->update_replica_state(replica.block_id, datanode_id, ReplicaState::kDeleted);
            if (mark.hasError()) {
                return folly::makeUnexpected(mark.error());
            }
        } else if ((replica.state == ReplicaState::kWriting ||
                    replica.state == ReplicaState::kStale) &&
                   is_reported && report->second.generation_stamp == replica.generation_stamp) {
            auto mark = store_->update_replica_state(
                replica.block_id, datanode_id, ReplicaState::kFinalized);
            if (mark.hasError()) {
                return folly::makeUnexpected(mark.error());
            }
        } else if (replica.state == ReplicaState::kFinalized &&
                   ((!is_reported && full_report) ||
                    (is_reported && report->second.generation_stamp != replica.generation_stamp))) {
            auto mark =
                store_->update_replica_state(replica.block_id, datanode_id, ReplicaState::kStale);
            if (mark.hasError()) {
                return folly::makeUnexpected(mark.error());
            }
        }
    }
    return pl::Void{};
}

} // namespace pl::minidfs
