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

#include <cstdint>

namespace pl::dfs {

static constexpr uint32_t kBlockMagic = 0;
static constexpr uint32_t kMaxChunkNum = 256;
static constexpr uint8_t kDataVersion = 1;

#pragma pack(push, r, 1)
struct BlockHeader {
    uint32_t magic = kBlockMagic;
    uint8_t version = kDataVersion;
    uint32_t len = 0;
    CompressionType compress_type = CompressionType::NONE;
    uint32_t compress_chunk = 1 * MB; // 将一个block切分成小的chunk，每个chunk的大小
    uint64_t inode = UINT64_MAX;
    uint64_t block_id = UINT64_MAX;
    uint32_t block_idx = UINT32_MAX;
    uint32_t compress_index[kMaxChunkNum];
};
#pragma pack(pop, r)

struct Block {
    uint8_t version = kDataVersion;
    uint64_t inode = UINT64_MAX;
    uint64_t block_id = UINT64_MAX;
    uint32_t block_idx = UINT32_MAX;
};

class BlockManager {};

} // namespace pl::dfs
