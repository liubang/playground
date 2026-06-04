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

#include "cpp/pl/sstv2/metadata/metadata_section.h"

#include <algorithm>
#include <cstring>

#include "absl/crc/crc32c.h"
#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::metadata {

void MetadataSection::put(std::string_view key, std::string_view value) {
    // Replace existing key if present
    for (auto& [k, v] : entries_) {
        if (k == key) {
            v = std::string(value);
            return;
        }
    }
    entries_.emplace_back(std::string(key), std::string(value));
}

void MetadataSection::put_uint64(std::string_view key, uint64_t value) {
    std::byte buf[8];
    encode::encode_fixed<uint64_t>(value, buf);
    put(key, std::string_view(reinterpret_cast<const char*>(buf), 8));
}

void MetadataSection::put_uint32(std::string_view key, uint32_t value) {
    std::byte buf[4];
    encode::encode_fixed<uint32_t>(value, buf);
    put(key, std::string_view(reinterpret_cast<const char*>(buf), 4));
}

void MetadataSection::put_uint16(std::string_view key, uint16_t value) {
    std::byte buf[2];
    encode::encode_fixed<uint16_t>(value, buf);
    put(key, std::string_view(reinterpret_cast<const char*>(buf), 2));
}

std::optional<std::string_view> MetadataSection::get(std::string_view key) const {
    for (const auto& [k, v] : entries_) {
        if (k == key) {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> MetadataSection::get_uint64(std::string_view key) const {
    auto val = get(key);
    if (!val || val->size() != 8) {
        return std::nullopt;
    }
    return encode::decode_fixed<uint64_t>(reinterpret_cast<const std::byte*>(val->data()));
}

std::optional<uint32_t> MetadataSection::get_uint32(std::string_view key) const {
    auto val = get(key);
    if (!val || val->size() != 4) {
        return std::nullopt;
    }
    return encode::decode_fixed<uint32_t>(reinterpret_cast<const std::byte*>(val->data()));
}

std::optional<uint16_t> MetadataSection::get_uint16(std::string_view key) const {
    auto val = get(key);
    if (!val || val->size() != 2) {
        return std::nullopt;
    }
    return encode::decode_fixed<uint16_t>(reinterpret_cast<const std::byte*>(val->data()));
}

std::string MetadataSection::serialize(uint32_t section_magic) const {
    // First pass: compute payload size (all TLV pairs)
    size_t payload_size = 0;
    for (const auto& [k, v] : entries_) {
        std::byte tmp[encode::Varints::kMaxVarint64Bytes];
        payload_size += encode::Varints::encode_uint32(static_cast<uint32_t>(k.size()), tmp);
        payload_size += k.size();
        payload_size += encode::Varints::encode_uint32(static_cast<uint32_t>(v.size()), tmp);
        payload_size += v.size();
    }

    // Total: magic(4) + length(4) + payload + crc(4)
    std::string result;
    result.resize(4 + 4 + payload_size + 4);
    auto* dst = reinterpret_cast<std::byte*>(result.data());
    size_t off = 0;

    // Write magic
    encode::encode_fixed<uint32_t>(section_magic, dst + off);
    off += 4;

    // Write section length (payload + crc)
    encode::encode_fixed<uint32_t>(static_cast<uint32_t>(payload_size + 4), dst + off);
    off += 4;

    // Write TLV pairs
    for (const auto& [k, v] : entries_) {
        off += encode::Varints::encode_uint32(static_cast<uint32_t>(k.size()), dst + off);
        std::memcpy(dst + off, k.data(), k.size());
        off += k.size();
        off += encode::Varints::encode_uint32(static_cast<uint32_t>(v.size()), dst + off);
        std::memcpy(dst + off, v.data(), v.size());
        off += v.size();
    }

    // Compute CRC32C over magic + length + payload
    auto crc = absl::ComputeCrc32c(std::string_view(result.data(), off));
    encode::encode_fixed<uint32_t>(static_cast<uint32_t>(crc), dst + off);

    return result;
}

absl::StatusOr<MetadataSection> MetadataSection::deserialize(std::span<const std::byte> data,
                                                             uint32_t expected_magic) {
    // Minimum size: magic(4) + length(4) + crc(4)
    if (data.size() < 12) {
        return absl::InvalidArgumentError("metadata section too small");
    }

    size_t off = 0;
    uint32_t magic = encode::decode_fixed<uint32_t>(data.data() + off);
    off += 4;

    if (magic != expected_magic) {
        return absl::InvalidArgumentError("metadata section magic mismatch");
    }

    uint32_t section_length = encode::decode_fixed<uint32_t>(data.data() + off);
    off += 4;

    if (data.size() < 8 + section_length) {
        return absl::InvalidArgumentError("metadata section truncated");
    }

    // CRC is the last 4 bytes of the section
    size_t crc_offset = 8 + section_length - 4;
    auto expected_crc = absl::crc32c_t{encode::decode_fixed<uint32_t>(data.data() + crc_offset)};
    auto actual_crc = absl::ComputeCrc32c(
        std::string_view(reinterpret_cast<const char*>(data.data()), crc_offset));

    if (expected_crc != actual_crc) {
        return absl::DataLossError("metadata section checksum mismatch");
    }

    // Parse TLV pairs
    size_t payload_end = crc_offset;
    MetadataSection section;

    while (off < payload_end) {
        auto [key_len, key_len_bytes] = encode::Varints::decode_uint32(data.subspan(off));
        off += key_len_bytes;
        if (off + key_len > payload_end) {
            return absl::InvalidArgumentError("metadata section key truncated");
        }
        std::string key(reinterpret_cast<const char*>(data.data() + off), key_len);
        off += key_len;

        auto [val_len, val_len_bytes] = encode::Varints::decode_uint32(data.subspan(off));
        off += val_len_bytes;
        if (off + val_len > payload_end) {
            return absl::InvalidArgumentError("metadata section value truncated");
        }
        std::string value(reinterpret_cast<const char*>(data.data() + off), val_len);
        off += val_len;

        section.entries_.emplace_back(std::move(key), std::move(value));
    }

    return section;
}

bool MetadataSection::empty() const {
    return entries_.empty();
}

size_t MetadataSection::size() const {
    return entries_.size();
}

} // namespace pl::sstv2::metadata
