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

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "cpp/pl/minidfs/common/block_token.h"
#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

class PlacementManager;

struct ReportedBlock {
    uint64_t block_id = 0;
    uint64_t generation_stamp = 0;
    uint64_t length = 0;
};

using TruncateReplicaFunc =
    std::function<pl::Result<pl::Void>(const BlockReplica& replica, uint64_t length)>;

// BlockManager — manages block lifecycle on the NameNode side.
//
// Responsibilities:
//   - Allocate new blocks for file writes (with placement)
//   - Commit blocks when write completes
//   - Manage generation stamps (monotonically increasing)
//   - Handle block state transitions
//   - Provide located-block info for reads

class BlockManager {
public:
    BlockManager(MetadataStore* store, PlacementManager* placement, std::string token_secret);
    ~BlockManager() = default;

    BlockManager(const BlockManager&) = delete;
    BlockManager& operator=(const BlockManager&) = delete;

    /// Allocate a new block for an inode. Selects datanodes via PlacementManager.
    /// Returns the allocated LocatedBlock with target datanode locations.
    pl::Result<LocatedBlock> allocate_block(uint64_t inode_id,
                                            uint32_t block_index,
                                            uint32_t replication);

    /// Commit a block after successful write. Updates length and state.
    pl::Result<pl::Void> commit_block(uint64_t block_id,
                                      uint64_t length,
                                      uint64_t generation_stamp,
                                      const std::vector<uint64_t>& finalized_datanode_ids = {});

    /// Get located blocks for reading a file (all committed blocks with locations).
    pl::Result<std::vector<LocatedBlock>> get_located_blocks(uint64_t inode_id);

    /// Report a corrupt block replica from a datanode.
    pl::Result<pl::Void> report_corrupt_replica(uint64_t block_id, uint64_t datanode_id);

    /// Invalidate all blocks belonging to an inode (mark kDeleted, remove replicas).
    /// Must be called before the inode is deleted from the namespace.
    pl::Result<pl::Void> invalidate_blocks(uint64_t inode_id);

    /// Recover an abandoned file write. Keeps committed blocks, invalidates
    /// allocating blocks, and returns the length of the readable prefix.
    pl::Result<uint64_t> recover_file(uint64_t inode_id);

    /// Shrink a closed file's blocks to new_length. Tail blocks are invalidated
    /// and the last retained block is physically truncated on its replicas.
    pl::Result<pl::Void> truncate_file(uint64_t inode_id,
                                       uint64_t new_length,
                                       const TruncateReplicaFunc& truncate_replica);

    /// Update the desired replica count for every live block in a file.
    pl::Result<pl::Void> set_replication(uint64_t inode_id, uint32_t replication);

    /// Return pending block deletion commands for a datanode.
    pl::Result<std::vector<BlockMeta>> get_blocks_to_delete(uint64_t datanode_id);

    /// Mark replicas no longer reported by a datanode as deleted.
    pl::Result<pl::Void> reconcile_block_report(uint64_t datanode_id,
                                                const std::vector<ReportedBlock>& reported_blocks,
                                                bool full_report);

    /// Generate a new unique generation stamp.
    pl::Result<uint64_t> next_generation_stamp();

private:
    pl::Result<pl::Void> invalidate_block(BlockMeta* block);
    protocol::BlockTokenProto issue_block_token(uint64_t block_id,
                                                uint64_t generation_stamp,
                                                uint64_t inode_id,
                                                uint32_t block_index,
                                                uint32_t permissions) const;

    MetadataStore* store_;
    PlacementManager* placement_;
    std::string token_secret_;
};

} // namespace pl::minidfs
