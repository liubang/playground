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
// Created: 2026/06/04 13:06

#include "cpp/pl/sstv2/block/rowkey_bitmap.h"

namespace pl::sstv2::block {

void RowKeyBitmapBuilder::add(bool is_same_as_prev) {
    bits_.push_back(is_same_as_prev);
}

std::string RowKeyBitmapBuilder::build() const {
    size_t num_bytes = (bits_.size() + 7) / 8;
    std::string output(num_bytes, '\0');
    for (size_t i = 0; i < bits_.size(); ++i) {
        if (bits_[i]) {
            output[i / 8] |= static_cast<char>(1 << (i % 8));
        }
    }
    return output;
}

size_t RowKeyBitmapBuilder::count() const {
    return bits_.size();
}

RowKeyBitmapReader::RowKeyBitmapReader(std::span<const std::byte> data, size_t num_rows)
    : data_(data), num_rows_(num_rows) {}

bool RowKeyBitmapReader::is_duplicate(size_t row_idx) const {
    if (row_idx >= num_rows_) {
        return false;
    }
    auto byte_val = static_cast<uint8_t>(data_[row_idx / 8]);
    return (byte_val >> (row_idx % 8)) & 1;
}

} // namespace pl::sstv2::block
