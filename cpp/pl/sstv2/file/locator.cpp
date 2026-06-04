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
// Created: 2026/06/04 22:26

#include "cpp/pl/sstv2/file/locator.h"

#include <array>
#include <cstddef>
#include <string>

#include "absl/crc/crc32c.h"
#include "absl/status/status.h"
#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"
#include "cpp/pl/sstv2/types/data_type.h"

namespace pl::sstv2::file {
namespace {

void AppendVarint64(uint64_t value, std::string& out) {
    std::array<std::byte, encode::Varints::kMaxVarint64Bytes> tmp{};
    const size_t n = encode::Varints::encode_uint64(value, tmp.data());
    out.append(reinterpret_cast<const char*>(tmp.data()), n);
}

void AppendFixed32(uint32_t value, std::string& out) {
    std::array<std::byte, sizeof(uint32_t)> tmp{};
    encode::encode_fixed<uint32_t>(value, tmp.data());
    out.append(reinterpret_cast<const char*>(tmp.data()), tmp.size());
}

void AppendFixed64(uint64_t value, std::string& out) {
    std::array<std::byte, sizeof(uint64_t)> tmp{};
    encode::encode_fixed<uint64_t>(value, tmp.data());
    out.append(reinterpret_cast<const char*>(tmp.data()), tmp.size());
}

absl::StatusOr<uint64_t> ReadVarint64(std::span<const std::byte> bytes, size_t& pos) {
    auto [value, consumed] = encode::Varints::decode_uint64(bytes.subspan(pos));
    if (consumed == 0 || pos + consumed > bytes.size()) {
        return absl::InvalidArgumentError("truncated locator varint");
    }
    pos += consumed;
    return value;
}

uint64_t SectionChecksum(std::string_view bytes) {
    std::string copy(bytes);
    std::fill(copy.begin() + 4, copy.begin() + 12, '\0');
    return static_cast<uint64_t>(static_cast<uint32_t>(absl::ComputeCrc32c(copy)));
}

} // namespace

void Locator::set_uint64(std::string key, uint64_t value) {
    entries_[std::move(key)] = value;
}

std::optional<uint64_t> Locator::find_uint64(std::string_view key) const {
    auto it = entries_.find(std::string(key));
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void Locator::set_section(std::string_view name, SectionLocation location) {
    set_uint64(std::string(name) + "_Offset", location.offset);
    set_uint64(std::string(name) + "_Length", location.length);
}

std::optional<SectionLocation> Locator::find_section(std::string_view name) const {
    auto offset = find_uint64(std::string(name) + "_Offset");
    auto length = find_uint64(std::string(name) + "_Length");
    if (!offset.has_value() || !length.has_value()) {
        return std::nullopt;
    }
    return SectionLocation{.offset = *offset, .length = *length};
}

std::string Locator::encode() const {
    std::string out;
    AppendFixed32(kMagic, out);
    AppendFixed64(0, out);
    AppendVarint64(entries_.size(), out);
    for (const auto& [key, value] : entries_) {
        AppendVarint64(key.size(), out);
        out.append(key);
        out.push_back(static_cast<char>(types::DataType::kUint64));
        AppendFixed64(value, out);
    }
    encode::encode_fixed<uint64_t>(SectionChecksum(out),
                                   reinterpret_cast<std::byte*>(out.data()) + 4);
    return out;
}

absl::StatusOr<Locator> Locator::decode(std::span<const std::byte> bytes) {
    if (bytes.size() < 12) {
        return absl::InvalidArgumentError("locator too small");
    }
    const uint32_t magic = encode::decode_fixed<uint32_t>(bytes.data());
    if (magic != kMagic) {
        return absl::InvalidArgumentError("invalid locator magic");
    }
    const uint64_t checksum = encode::decode_fixed<uint64_t>(bytes.data() + 4);
    std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (checksum != SectionChecksum(view)) {
        return absl::DataLossError("locator checksum mismatch");
    }

    size_t pos = 12;
    auto count_or = ReadVarint64(bytes, pos);
    if (!count_or.ok()) {
        return count_or.status();
    }

    Locator locator;
    for (uint64_t i = 0; i < *count_or; ++i) {
        auto key_len_or = ReadVarint64(bytes, pos);
        if (!key_len_or.ok()) {
            return key_len_or.status();
        }
        if (pos + *key_len_or + 1 + sizeof(uint64_t) > bytes.size()) {
            return absl::InvalidArgumentError("truncated locator entry");
        }
        std::string key(reinterpret_cast<const char*>(bytes.data() + pos), *key_len_or);
        pos += *key_len_or;
        const auto type = static_cast<types::DataType>(bytes[pos++]);
        if (type != types::DataType::kUint64) {
            return absl::InvalidArgumentError("locator value is not UInt64");
        }
        const uint64_t value = encode::decode_fixed<uint64_t>(bytes.data() + pos);
        pos += sizeof(uint64_t);
        locator.set_uint64(std::move(key), value);
    }
    if (pos != bytes.size()) {
        return absl::InvalidArgumentError("locator has trailing bytes");
    }
    return locator;
}

const std::map<std::string, uint64_t>& Locator::entries() const {
    return entries_;
}

} // namespace pl::sstv2::file
