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

struct OffsetEntry {
    uint32_t offset = 0;
    uint32_t length = 0;
};

class OffsetTableBuilder {
public:
    void add_entry(uint32_t offset, uint32_t length);
    std::string build() const;
    size_t count() const;

private:
    std::vector<OffsetEntry> entries_;
};

class OffsetTableReader {
public:
    OffsetTableReader() = default;
    OffsetTableReader(std::span<const std::byte> data, size_t count);

    OffsetEntry get(size_t idx) const;
    size_t count() const;

private:
    std::vector<OffsetEntry> entries_;
};

} // namespace pl::sstv2::block
