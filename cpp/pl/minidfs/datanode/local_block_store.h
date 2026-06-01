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
// Created: 2026/05/10 17:45

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/minidfs/datanode/block_format.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// BlockInfo — lightweight descriptor for a stored block on disk.

struct BlockInfo {
    uint64_t block_id = 0;
    uint64_t generation_stamp = 0;
    uint64_t length = 0; // total file size on disk (header + data)
};

// LocalBlockStore — manages block files on the local filesystem.
//
// Directory layout under the configured storage root:
//
//   <root>/
//     current/        — finalized, committed blocks
//       blk_<id>_<gs>.blk
//     tmp/            — blocks currently being written (pipeline receive)
//       blk_<id>_<gs>.blk
//     trash/          — soft-deleted blocks awaiting async cleanup
//       blk_<id>_<gs>.blk
//
// Lifecycle:
//   1. create_block()   → creates file in tmp/
//   2. append_chunk()   → appends data to the tmp file
//   3. finalize_block() → moves from tmp/ to current/
//   4. delete_block()   → moves from current/ to trash/
//   5. purge_trash()    → permanently removes files from trash/
//
// Thread safety:
//   The store uses a mutex for metadata operations (create, finalize, delete).
//   Concurrent reads of finalized blocks are safe without locking.

class LocalBlockStore {
public:
    struct Config {
        std::string storage_root;
        uint64_t reserved_bytes = 1 * kGB; // reserved space, refuse writes if below
    };

    explicit LocalBlockStore(Config config);
    ~LocalBlockStore() = default;

    LocalBlockStore(const LocalBlockStore&) = delete;
    LocalBlockStore& operator=(const LocalBlockStore&) = delete;

    /// Initialize storage directories. Must be called once before use.
    pl::Result<pl::Void> init();

    /// Create a new block in tmp/ and write the initial header.
    /// The block enters the "writing" state.
    pl::Result<pl::Void> create_block(uint64_t block_id,
                                      uint64_t inode_id,
                                      uint32_t block_index,
                                      uint64_t generation_stamp);

    /// Append a data chunk to a block currently in tmp/.
    /// Updates the header's chunk metadata and writes data after the header.
    /// Returns the updated data length (uncompressed cumulative).
    ///
    /// Ordering: chunk_index must equal the current chunk_count (sequential writes).
    /// Idempotency: if chunk_index == chunk_count - 1 and CRC matches the last
    /// written chunk, the call is treated as a successful retry (no re-append).
    pl::Result<uint64_t> append_chunk(uint64_t block_id,
                                      uint64_t generation_stamp,
                                      const void* data,
                                      uint32_t size,
                                      uint32_t chunk_index);

    /// Finalize a block: verify checksums, update header, move from tmp/ to current/.
    pl::Result<pl::Void> finalize_block(uint64_t block_id, uint64_t generation_stamp);

    /// Soft-delete a block: move from current/ to trash/.
    pl::Result<pl::Void> delete_block(uint64_t block_id, uint64_t generation_stamp);

    /// Shrink a finalized block and rebuild its checksums. Idempotent for retries.
    pl::Result<pl::Void> truncate_block(uint64_t block_id,
                                        uint64_t generation_stamp,
                                        uint64_t length);

    /// Permanently remove all files in trash/.
    pl::Result<uint32_t> purge_trash();

    /// Read the full data region of a finalized block.
    /// Returns raw bytes (header excluded).
    pl::Result<std::string> read_block_data(uint64_t block_id, uint64_t generation_stamp);

    /// Read a specific chunk from a finalized block.
    pl::Result<std::string> read_chunk(uint64_t block_id,
                                       uint64_t generation_stamp,
                                       uint32_t chunk_index);

    /// Verify CRC32C integrity of a finalized block.
    pl::Result<bool> verify_block(uint64_t block_id, uint64_t generation_stamp);

    /// Report all finalized blocks (in current/).
    pl::Result<std::vector<BlockInfo>> report_blocks();

    /// Check if a block exists in current/.
    bool has_block(uint64_t block_id, uint64_t generation_stamp) const;

    /// Get available disk space in the storage root.
    pl::Result<uint64_t> available_bytes() const;

    /// Get the storage root path.
    const std::string& storage_root() const { return config_.storage_root; }

private:
    /// Generate the canonical filename for a block.
    static std::string block_filename(uint64_t block_id, uint64_t generation_stamp);

    /// Get full path for a block in a given subdirectory.
    std::filesystem::path block_path(std::string_view subdir,
                                     uint64_t block_id,
                                     uint64_t generation_stamp) const;

    /// Read the BlockHeader from a file.
    pl::Result<BlockHeader> read_header(const std::filesystem::path& path) const;

    /// Verify CRC32C integrity for a block file at an explicit path.
    pl::Result<bool> verify_block_file(const std::filesystem::path& path) const;

    /// Write the BlockHeader to a file (at offset 0).
    pl::Result<pl::Void> write_header(const std::filesystem::path& path,
                                      const BlockHeader& header) const;

    Config config_;
    std::filesystem::path root_path_;
    std::filesystem::path tmp_path_;
    std::filesystem::path current_path_;
    std::filesystem::path trash_path_;
    mutable std::mutex mu_;
};

} // namespace pl::minidfs
