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
#include <string_view>
#include <vector>

namespace pl::sstv2::index {

// IndexBlockWriter serializes a single index block. An index block stores a
// batch of routing entries, each pointing to a child block (either a data
// block or a lower-level index block).
//
// On-disk layout:
//   [magic:4B][entry_count:varint][entries...][checksum:4B]
//
// Each entry is encoded as:
//   [key_len:varint][last_key bytes][offset:varint u64]
//   [size:varint u32][sub_column_flags:varint u32]
class IndexBlockWriter {
public:
    IndexBlockWriter() = default;

    // Append a routing entry. last_key is the last row_key contained in the
    // pointed-to block; offset/size locate the block in the file.
    void add_entry(std::string_view last_key,
                   uint64_t offset,
                   uint32_t size,
                   uint32_t sub_column_flags = 0);

    // Serialize the index block, including the header (magic + entry_count)
    // and trailing checksum. Does not reset the builder.
    std::string finish() const;

    // Estimated serialized size in bytes.
    size_t estimated_size() const;

    // Number of entries currently buffered.
    size_t count() const;

    // True when no entries have been added.
    bool empty() const;

    // The last_key of the most recently added entry. Valid only when !empty().
    std::string_view last_key() const;

    // Clear all buffered entries.
    void reset();

private:
    struct Entry {
        std::string last_key;
        uint64_t offset = 0;
        uint32_t size = 0;
        uint32_t sub_column_flags = 0;
    };

    std::vector<Entry> entries_;
    size_t estimated_size_ = 0;
};

} // namespace pl::sstv2::index
