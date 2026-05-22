// Copyright (c) 2025 The Authors. All rights reserved.
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
// Created: 2025/05/10 15:30

#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "cpp/pl/minidfs/common/checksum.h"
#include "cpp/pl/minidfs/common/compression.h"
#include "cpp/pl/minidfs/common/constants.h"

namespace pl::minidfs {

// ============================================================================
// BlockHeader — on-disk binary format for a block file.
//
// Layout: [BlockHeader (fixed size)] [Data Region (variable)]
//
// The data region contains `chunk_count` compressed (or raw) chunks.
// Each chunk's offset within the data region is recorded in `chunk_offsets`.
// Each chunk's CRC32C is recorded in `chunk_checksums`.
//
// The `block_checksum` covers the entire data region (all chunks concatenated).
// ============================================================================

#pragma pack(push, 1)
struct BlockHeader {
    uint32_t magic = kBlockMagic;
    uint32_t version = kBlockFormatVersion;
    uint64_t block_id = 0;
    uint64_t inode_id = 0;
    uint32_t block_index = 0;
    uint64_t generation_stamp = 0;
    uint64_t data_length = 0;      // uncompressed data length
    uint32_t compression_type = 0; // CompressionType as uint32
    uint32_t chunk_size = kDefaultChunkSize;
    uint32_t chunk_count = 0;
    uint32_t checksum_type = static_cast<uint32_t>(ChecksumType::kCRC32C);
    uint32_t block_checksum = 0; // CRC32C of entire data region
    uint32_t chunk_offsets[kMaxChunkCount] = {};
    uint32_t chunk_checksums[kMaxChunkCount] = {};
    uint8_t reserved[32] = {};
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<BlockHeader>,
              "BlockHeader must be trivially copyable for direct I/O.");

// Header size is fixed regardless of actual chunk_count.
inline constexpr size_t kBlockHeaderSize = sizeof(BlockHeader);

// ============================================================================
// Utility functions
// ============================================================================

/// Validate that a BlockHeader has the correct magic and version.
[[nodiscard]] inline bool validate_block_header(const BlockHeader& header) {
    return header.magic == kBlockMagic && header.version == kBlockFormatVersion;
}

/// Initialize a BlockHeader for a new block.
inline void init_block_header(BlockHeader& header,
                              uint64_t block_id,
                              uint64_t inode_id,
                              uint32_t block_index,
                              uint64_t generation_stamp,
                              CompressionType compression = CompressionType::kNone,
                              uint32_t chunk_size = kDefaultChunkSize) {
    std::memset(&header, 0, sizeof(header));
    header.magic = kBlockMagic;
    header.version = kBlockFormatVersion;
    header.block_id = block_id;
    header.inode_id = inode_id;
    header.block_index = block_index;
    header.generation_stamp = generation_stamp;
    header.compression_type = static_cast<uint32_t>(compression);
    header.chunk_size = chunk_size;
    header.checksum_type = static_cast<uint32_t>(ChecksumType::kCRC32C);
}

} // namespace pl::minidfs
