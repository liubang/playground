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
#include <string_view>

namespace pl::sstv2::block {

class DataTableBuilder {
public:
    // Append data, return the offset where it was written
    uint32_t append(std::string_view data);

    // Build and return the accumulated buffer
    std::string build() const;

    // Current size of the buffer
    size_t current_size() const;

private:
    std::string buffer_;
};

class DataTableReader {
public:
    DataTableReader() = default;
    explicit DataTableReader(std::span<const std::byte> data);

    // Get a string_view at the given offset and length
    std::string_view get(uint32_t offset, uint32_t length) const;

private:
    std::span<const std::byte> data_;
};

} // namespace pl::sstv2::block
