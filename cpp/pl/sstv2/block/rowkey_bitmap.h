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

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pl::sstv2::block {

class RowKeyBitmapBuilder {
public:
    // Add whether this row key is the same as the previous row's key
    void add(bool is_same_as_prev);

    // Build packed bit vector
    std::string build() const;

    size_t count() const;

private:
    std::vector<bool> bits_;
};

class RowKeyBitmapReader {
public:
    RowKeyBitmapReader() = default;
    RowKeyBitmapReader(std::span<const std::byte> data, size_t num_rows);

    // Check if row_idx has a duplicate row key (same as previous)
    bool is_duplicate(size_t row_idx) const;

private:
    std::span<const std::byte> data_;
    size_t num_rows_ = 0;
};

} // namespace pl::sstv2::block
