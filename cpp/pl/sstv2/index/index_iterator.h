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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace pl::sstv2::index {

// IndexIterator descends a multi-level B-tree index (produced by
// IndexTreeBuilder) over raw file data to locate data blocks. It supports
// forward range scans: seek() positions at the first data block whose last_key
// is >= the target, and next() advances to the following data block.
class IndexIterator {
public:
    // Open an iterator over the index tree rooted at root_offset. file_data is
    // the whole file (or at least a span covering all index blocks). Offsets
    // embedded in routing entries are interpreted relative to the start of
    // file_data. tree_height is the number of index levels.
    static absl::StatusOr<IndexIterator> open(std::span<const std::byte> file_data,
                                              uint64_t root_offset,
                                              uint8_t tree_height);

    // Position at the first data block with last_key >= target_key.
    absl::Status seek(std::string_view target_key);

    // Advance to the next data block.
    absl::Status next();

    // True when positioned at a valid data block.
    bool valid() const;

    // File offset / size of the data block at the current position. Only valid
    // when valid() is true.
    uint64_t current_block_offset() const;
    uint32_t current_block_size() const;

private:
    // A decoded routing entry within an index block.
    struct Entry {
        std::string last_key;
        uint64_t offset = 0;
        uint32_t size = 0;
        uint32_t sub_column_flags = 0;
    };

    // One level of the active descent path.
    struct PathNode {
        std::vector<Entry> entries;
        size_t pos = 0;
    };

    IndexIterator(std::span<const std::byte> file_data, uint64_t root_offset, uint8_t tree_height);

    // Parse the index block located at the given file offset into entries.
    static absl::Status parse_block(std::span<const std::byte> file_data,
                                    uint64_t offset,
                                    std::vector<Entry>* out);

    // Build the descent path so that, at every level, pos selects the first
    // entry whose last_key >= target_key (or the last entry if none qualify).
    absl::Status descend(std::string_view target_key);

    // Advance the leaf cursor, re-descending sibling subtrees as needed.
    void advance_path();

    std::span<const std::byte> file_data_;
    uint64_t root_offset_ = 0;
    uint8_t tree_height_ = 0;

    // path_[0] is the root level; path_.back() is the leaf index level whose
    // entries point at data blocks.
    std::vector<PathNode> path_;
    bool valid_ = false;
};

} // namespace pl::sstv2::index
