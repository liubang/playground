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

#include "cpp/pl/sstv2/index/index_iterator.h"

#include "absl/crc/crc32c.h"
#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"
#include "cpp/pl/sstv2/types/constants.h"

namespace pl::sstv2::index {

IndexIterator::IndexIterator(std::span<const std::byte> file_data,
                             uint64_t root_offset,
                             uint8_t tree_height)
    : file_data_(file_data), root_offset_(root_offset), tree_height_(tree_height) {}

absl::StatusOr<IndexIterator> IndexIterator::open(std::span<const std::byte> file_data,
                                                  uint64_t root_offset,
                                                  uint8_t tree_height) {
    if (tree_height == 0) {
        return absl::InvalidArgumentError("index tree height must be positive");
    }
    if (root_offset >= file_data.size()) {
        return absl::OutOfRangeError("root offset out of range");
    }
    // Validate the root block is parseable up front.
    std::vector<Entry> root_entries;
    if (auto st = parse_block(file_data, root_offset, &root_entries); !st.ok()) {
        return st;
    }
    return IndexIterator(file_data, root_offset, tree_height);
}

absl::Status IndexIterator::parse_block(std::span<const std::byte> file_data,
                                        uint64_t offset,
                                        std::vector<Entry>* out) {
    using pl::sstv2::encode::decode_fixed;
    using pl::sstv2::encode::Varints;

    if (offset >= file_data.size()) {
        return absl::OutOfRangeError("index block offset out of range");
    }
    auto block = file_data.subspan(offset);
    // Minimum: magic(4) + entry_count varint(>=1) + checksum(4).
    if (block.size() < 9) {
        return absl::DataLossError("index block too small");
    }

    uint32_t magic = decode_fixed<uint32_t>(block.data());
    if (magic != types::kIndexBlockMagic) {
        return absl::DataLossError("invalid index block magic");
    }

    size_t pos = 4;
    auto [entry_count, n] = Varints::decode_uint32(block.subspan(pos));
    pos += n;

    out->clear();
    out->reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        auto [key_len, kn] = Varints::decode_uint32(block.subspan(pos));
        pos += kn;
        if (pos + key_len + 4 > block.size()) {
            return absl::DataLossError("index entry key out of range");
        }
        Entry e;
        e.last_key.assign(reinterpret_cast<const char*>(block.data() + pos), key_len);
        pos += key_len;

        auto [off, on] = Varints::decode_uint64(block.subspan(pos));
        pos += on;
        e.offset = off;

        auto [sz, sn] = Varints::decode_uint32(block.subspan(pos));
        pos += sn;
        e.size = sz;

        auto [flags, fn] = Varints::decode_uint32(block.subspan(pos));
        pos += fn;
        e.sub_column_flags = flags;

        out->push_back(std::move(e));
    }

    // Verify trailing checksum over [start, pos).
    if (pos + 4 > block.size()) {
        return absl::DataLossError("index block missing checksum");
    }
    std::string_view payload(reinterpret_cast<const char*>(block.data()), pos);
    auto crc = absl::ComputeCrc32c(payload);
    uint32_t stored = decode_fixed<uint32_t>(block.data() + pos);
    if (static_cast<uint32_t>(crc) != stored) {
        return absl::DataLossError("index block checksum mismatch");
    }
    return absl::OkStatus();
}

absl::Status IndexIterator::descend(std::string_view target_key) {
    path_.assign(tree_height_, PathNode{});
    valid_ = false;

    uint64_t offset = root_offset_;
    for (uint8_t level = 0; level < tree_height_; ++level) {
        if (auto st = parse_block(file_data_, offset, &path_[level].entries); !st.ok()) {
            return st;
        }
        const auto& entries = path_[level].entries;
        if (entries.empty()) {
            return absl::DataLossError("empty index block");
        }
        // First entry whose last_key >= target_key (binary search).
        size_t lo = 0;
        size_t hi = entries.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (std::string_view(entries[mid].last_key) < target_key) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        size_t idx = lo;
        if (idx >= entries.size()) {
            // No entry covers the target at this level: target is past the end
            // of this subtree. Mark position past the last entry.
            path_[level].pos = entries.size();
            return absl::OkStatus();
        }
        path_[level].pos = idx;
        offset = entries[idx].offset;
    }

    valid_ = true;
    return absl::OkStatus();
}

absl::Status IndexIterator::seek(std::string_view target_key) {
    return descend(target_key);
}

void IndexIterator::advance_path() {
    if (path_.empty()) {
        valid_ = false;
        return;
    }
    // Advance the leaf cursor first.
    size_t leaf = path_.size() - 1;
    ++path_[leaf].pos;
    if (path_[leaf].pos < path_[leaf].entries.size()) {
        valid_ = true;
        return;
    }

    // Leaf exhausted: walk up to find a level with a remaining sibling.
    size_t level = leaf;
    while (level > 0) {
        --level;
        ++path_[level].pos;
        if (path_[level].pos < path_[level].entries.size()) {
            // Re-descend to the leftmost leaf under the new subtree.
            uint64_t offset = path_[level].entries[path_[level].pos].offset;
            for (size_t l = level + 1; l < path_.size(); ++l) {
                if (auto st = parse_block(file_data_, offset, &path_[l].entries); !st.ok()) {
                    valid_ = false;
                    return;
                }
                path_[l].pos = 0;
                if (path_[l].entries.empty()) {
                    valid_ = false;
                    return;
                }
                offset = path_[l].entries[0].offset;
            }
            valid_ = true;
            return;
        }
    }

    // Walked off the root: no more data blocks.
    valid_ = false;
}

absl::Status IndexIterator::next() {
    if (!valid_) {
        return absl::FailedPreconditionError("iterator is not valid");
    }
    advance_path();
    return absl::OkStatus();
}

bool IndexIterator::valid() const {
    return valid_;
}

uint64_t IndexIterator::current_block_offset() const {
    const auto& leaf = path_.back();
    return leaf.entries[leaf.pos].offset;
}

uint32_t IndexIterator::current_block_size() const {
    const auto& leaf = path_.back();
    return leaf.entries[leaf.pos].size;
}

} // namespace pl::sstv2::index
