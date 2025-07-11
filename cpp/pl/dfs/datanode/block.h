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

#pragma once

#include "cpp/pl/dfs/common/common.h"
#include "cpp/pl/dfs/common/compression.h"
#include "cpp/pl/status/result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

namespace pl::dfs {

static constexpr uint32_t kBlockMagic = 0;
static constexpr uint32_t kMaxChunkNum = 256;
static constexpr uint8_t kDataVersion = 1;

#pragma pack(push, 1)
struct BlockHeader {
    uint32_t magic_code;     // magic code
    uint32_t version;        // version
    uint32_t len;            // block length
    uint32_t compress_type;  // compresstion type
    uint32_t compress_chunk; // split block to small chunk and compress each chunk individually
    uint64_t inode;          // inode
    uint64_t block_id;       // block_id
    uint32_t block_idx;      // block index
    uint32_t compress_index[kMaxChunkNum]; // each chun compressed offset
};
#pragma pack(pop)

static_assert(std::is_pod<BlockHeader>::value, "BlockHeader is not POD.");
static_assert(sizeof(BlockHeader) == 1064, "sizeof(BlockHeader) not equal 1064.");

struct Block {
    uint8_t version = kDataVersion;
    uint64_t inode = UINT64_MAX;
    uint64_t block_id = UINT64_MAX;
    uint32_t block_idx = UINT32_MAX;
};

class BlockManager {
public:
    BlockManager();

    ~BlockManager();

    pl::Result<std::string> read(uint64_t block_id, uint32_t offset, uint32_t len);

    pl::Result<uint32_t> write(
        uint64_t block_id, uint32_t offset, uint32_t len, uint64_t inode, uint32_t block_idx);

private:
};

using BlockManagerPtr = std::unique_ptr<BlockManager>;
using BlockManagerRef = std::shared_ptr<BlockManager>;

} // namespace pl::dfs
