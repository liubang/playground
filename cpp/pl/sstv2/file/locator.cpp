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
// Created: 2026/06/04 15:23

#include "cpp/pl/sstv2/file/locator.h"

#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::file {

void Locator::add(uint32_t section_type, uint64_t offset, uint64_t size) {
    entries_.push_back({section_type, offset, size});
}

std::optional<LocatorEntry> Locator::find(uint32_t section_type) const {
    for (const auto& entry : entries_) {
        if (entry.section_type == section_type) {
            return entry;
        }
    }
    return std::nullopt;
}

std::string Locator::serialize() const {
    using pl::sstv2::encode::encode_fixed;
    using pl::sstv2::encode::Varints;

    std::string buf;
    // Reserve a reasonable amount
    buf.resize(Varints::kMaxVarint32Bytes + entries_.size() * (4 + 2 * Varints::kMaxVarint64Bytes));

    auto* dst = reinterpret_cast<std::byte*>(buf.data());
    size_t pos = 0;

    // entry count as varint
    pos += Varints::encode_uint32(static_cast<uint32_t>(entries_.size()), dst + pos);

    for (const auto& entry : entries_) {
        encode_fixed<uint32_t>(entry.section_type, dst + pos);
        pos += 4;
        pos += Varints::encode_uint64(entry.offset, dst + pos);
        pos += Varints::encode_uint64(entry.size, dst + pos);
    }

    buf.resize(pos);
    return buf;
}

absl::StatusOr<Locator> Locator::deserialize(std::span<const std::byte> data) {
    using pl::sstv2::encode::decode_fixed;
    using pl::sstv2::encode::Varints;

    if (data.empty()) {
        return absl::InvalidArgumentError("empty locator data");
    }

    size_t pos = 0;

    auto [count, count_bytes] = Varints::decode_uint32(data.subspan(pos));
    pos += count_bytes;

    Locator locator;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 4 > data.size()) {
            return absl::InvalidArgumentError("truncated locator entry");
        }
        uint32_t section_type = decode_fixed<uint32_t>(data.data() + pos);
        pos += 4;

        auto [offset, offset_bytes] = Varints::decode_uint64(data.subspan(pos));
        pos += offset_bytes;

        auto [size, size_bytes] = Varints::decode_uint64(data.subspan(pos));
        pos += size_bytes;

        locator.add(section_type, offset, size);
    }

    return locator;
}

const std::vector<LocatorEntry>& Locator::entries() const {
    return entries_;
}

} // namespace pl::sstv2::file
