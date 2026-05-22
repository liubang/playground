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

#include <atomic>
#include <cstdint>
#include <vector>

#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

class PlacementManager;

// ============================================================================
// BlockManager — manages block lifecycle on the NameNode side.
//
// Responsibilities:
//   - Allocate new blocks for file writes (with placement)
//   - Commit blocks when write completes
//   - Manage generation stamps (monotonically increasing)
//   - Handle block state transitions
//   - Provide located-block info for reads
// ============================================================================

class BlockManager {
public:
    BlockManager(MetadataStore* store, PlacementManager* placement);
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
                                      uint64_t generation_stamp);

    /// Get located blocks for reading a file (all committed blocks with locations).
    pl::Result<std::vector<LocatedBlock>> get_located_blocks(uint64_t inode_id);

    /// Report a corrupt block replica from a datanode.
    pl::Result<pl::Void> report_corrupt_replica(uint64_t block_id, uint64_t datanode_id);

    /// Generate a new unique generation stamp.
    uint64_t next_generation_stamp();

private:
    MetadataStore* store_;
    PlacementManager* placement_;
    std::atomic<uint64_t> generation_stamp_counter_{0};
};

} // namespace pl::minidfs
