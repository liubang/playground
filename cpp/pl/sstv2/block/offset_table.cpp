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

#include "cpp/pl/sstv2/block/offset_table.h"

#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::block {

void OffsetTableBuilder::add_entry(uint32_t offset, uint32_t length) {
    entries_.push_back({offset, length});
}

std::string OffsetTableBuilder::build() const {
    using pl::sstv2::encode::Varints;
    std::string output;
    // Reserve a reasonable estimate
    output.reserve(entries_.size() * 2 * Varints::kMaxVarint32Bytes);

    std::byte buf[Varints::kMaxVarint32Bytes];
    for (const auto& entry : entries_) {
        size_t n = Varints::encode_uint32(entry.offset, buf);
        output.append(reinterpret_cast<const char*>(buf), n);
        n = Varints::encode_uint32(entry.length, buf);
        output.append(reinterpret_cast<const char*>(buf), n);
    }
    return output;
}

size_t OffsetTableBuilder::count() const {
    return entries_.size();
}

OffsetTableReader::OffsetTableReader(std::span<const std::byte> data, size_t count) {
    using pl::sstv2::encode::Varints;
    entries_.reserve(count);
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
        auto [offset, n1] = Varints::decode_uint32(data.subspan(pos));
        pos += n1;
        auto [length, n2] = Varints::decode_uint32(data.subspan(pos));
        pos += n2;
        entries_.push_back({offset, length});
    }
}

OffsetEntry OffsetTableReader::get(size_t idx) const {
    return entries_[idx];
}

size_t OffsetTableReader::count() const {
    return entries_.size();
}

} // namespace pl::sstv2::block
