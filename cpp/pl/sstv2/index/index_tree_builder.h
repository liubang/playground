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
// Created: 2026/06/04 14:01

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cpp/pl/sstv2/index/index_block_writer.h"
#include "cpp/pl/sstv2/types/constants.h"

namespace pl::sstv2::index {

struct IndexTreeOptions {
    // Maximum serialized size of a single index block before it is flushed.
    size_t index_block_size = types::kDefaultIndexBlockSize;

    // Absolute file offset at which the first generated index block will be
    // written. All offsets reported in the build result (including
    // root_offset and the offsets embedded in routing entries) are absolute
    // file offsets, so this lets the builder reference its own index blocks.
    uint64_t index_region_offset = 0;
};

struct IndexBuildResult {
    // Serialized index blocks in post-order: leaf-level blocks first, then
    // higher levels, with the root index block last.
    std::vector<std::string> index_blocks;

    // Absolute file offset of the root index block.
    uint64_t root_offset = 0;

    // Number of index levels. 0 means no index was built (no data blocks).
    uint8_t tree_height = 0;
};

// IndexTreeBuilder constructs a multi-level B-tree index using post-order
// traversal. Data blocks are fed in key order via add_data_block(); leaf-level
// index blocks accumulate routing entries and flush when full, pushing their
// own routing entry up to the next level. This repeats until a single root
// block remains.
class IndexTreeBuilder {
public:
    explicit IndexTreeBuilder(IndexTreeOptions opts = IndexTreeOptions{});

    // Register a data block that has just been written to the file. last_key
    // is the last row_key in the block; offset/size locate it in the file.
    void add_data_block(std::string_view last_key, uint64_t offset, uint32_t size);

    // Finish building. Returns all generated index blocks (post-order), the
    // root block offset, and the tree height. May be called once.
    IndexBuildResult finish();

private:
    // One level of the in-progress B-tree.
    struct Level {
        IndexBlockWriter writer;
    };

    // Flush the writer at the given level into the output, then propagate the
    // resulting routing entry up to level + 1, creating it if needed.
    void flush_level(size_t level);

    // Append a routing entry to a given level, flushing it first if adding the
    // entry would exceed the configured block size.
    void add_to_level(size_t level, std::string_view last_key, uint64_t offset, uint32_t size);

    IndexTreeOptions opts_;
    std::vector<Level> levels_;

    // Index blocks generated so far, in flush (post-)order.
    std::vector<std::string> blocks_;

    // Running absolute offset where the next generated index block lands.
    uint64_t next_index_offset_ = 0;

    bool finished_ = false;
    bool has_data_ = false;
};

} // namespace pl::sstv2::index
