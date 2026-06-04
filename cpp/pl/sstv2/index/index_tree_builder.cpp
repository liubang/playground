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

#include "cpp/pl/sstv2/index/index_tree_builder.h"

#include <string>
#include <utility>

namespace pl::sstv2::index {

IndexTreeBuilder::IndexTreeBuilder(IndexTreeOptions opts)
    : opts_(opts), next_index_offset_(opts.index_region_offset) {}

void IndexTreeBuilder::add_data_block(std::string_view last_key, uint64_t offset, uint32_t size) {
    has_data_ = true;
    add_to_level(0, last_key, offset, size);
}

void IndexTreeBuilder::add_to_level(size_t level,
                                    std::string_view last_key,
                                    uint64_t offset,
                                    uint32_t size) {
    if (level >= levels_.size()) {
        levels_.resize(level + 1);
    }

    auto& writer = levels_[level].writer;
    // Flush before adding if this entry would push the block past the limit.
    // We require a minimum fan-out of 2 entries per block: otherwise, with a
    // pathologically small index_block_size, every block would hold a single
    // entry and the tree could never reduce its node count, never converging
    // to a single root.
    if (writer.count() >= 2 &&
        writer.estimated_size() + last_key.size() + 32 > opts_.index_block_size) {
        flush_level(level);
    }
    levels_[level].writer.add_entry(last_key, offset, size);
}

void IndexTreeBuilder::flush_level(size_t level) {
    auto& writer = levels_[level].writer;
    if (writer.empty()) {
        return;
    }

    std::string block = writer.finish();
    std::string promote_key(writer.last_key());
    uint64_t block_offset = next_index_offset_;
    auto block_size = static_cast<uint32_t>(block.size());

    next_index_offset_ += block.size();
    blocks_.push_back(std::move(block));
    writer.reset();

    // Promote a routing entry for the flushed block to the next level up.
    add_to_level(level + 1, promote_key, block_offset, block_size);
}

IndexBuildResult IndexTreeBuilder::finish() {
    IndexBuildResult result;
    if (finished_ || !has_data_) {
        finished_ = true;
        return result;
    }
    finished_ = true;

    // Flush levels from the bottom up. Flushing a level promotes a routing
    // entry to the level above (possibly creating it), so we walk upward.
    // We stop once we reach the topmost level whose writer holds a single
    // routing entry: that block is the root and must not be promoted further
    // (otherwise we would keep creating empty single-entry parent levels).
    for (size_t level = 0; level < levels_.size(); ++level) {
        auto& writer = levels_[level].writer;
        if (writer.empty()) {
            continue;
        }
        // If this is the last level and it already holds exactly one entry,
        // it is the root: flush it without promoting.
        bool is_top = (level + 1 == levels_.size());
        if (is_top && writer.count() == 1) {
            std::string block = writer.finish();
            result.root_offset = next_index_offset_;
            next_index_offset_ += block.size();
            blocks_.push_back(std::move(block));
            writer.reset();
            result.tree_height = static_cast<uint8_t>(level + 1);
            result.index_blocks = std::move(blocks_);
            return result;
        }
        flush_level(level);
    }

    // Fallthrough: the last flush produced the root as the final block.
    result.tree_height = static_cast<uint8_t>(levels_.size());
    result.root_offset = next_index_offset_ - blocks_.back().size();
    result.index_blocks = std::move(blocks_);
    return result;
}

} // namespace pl::sstv2::index
