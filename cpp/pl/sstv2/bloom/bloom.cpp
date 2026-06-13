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
// Created: 2026/06/05 22:09

#include "cpp/pl/sstv2/bloom/bloom.h"

#include <algorithm>
#include <cmath>

#include "absl/hash/hash.h"
#include "absl/status/status.h"
#include "cpp/pl/bloom/bloom.h"
#include "cpp/pl/sstv2/codec/checksum.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/codec/value_comparable.h"

namespace pl::sstv2::bloom {

namespace {

uint32_t hash_key(std::string_view key) {
    return static_cast<uint32_t>(absl::Hash<std::string_view>{}(key));
}

void encode_header(const Header& header, std::string* dst) {
    codec::append_fixed32(dst, header.magic);
    codec::append_fixed32(dst, header.version);
    codec::append_fixed32(dst, header.hash_count);
    codec::append_fixed64(dst, header.bit_count);
    codec::append_fixed64(dst, header.row_count);
    codec::append_fixed64(dst, header.checksum);
}

Header decode_header(std::string_view input) {
    Header h;
    h.magic = codec::read_fixed32(input, 0);
    h.version = codec::read_fixed32(input, 4);
    h.hash_count = codec::read_fixed32(input, 8);
    h.bit_count = codec::read_fixed64(input, 12);
    h.row_count = codec::read_fixed64(input, 20);
    h.checksum = codec::read_fixed64(input, 28);
    return h;
}

} // namespace

Builder::Builder(int bits_per_key) : bits_per_key_(std::max(1, bits_per_key)) {}

absl::Status Builder::add(const types::InternalRow& row, types::InternalSchema::ConstRef schema) {
    auto all_key = codec::make_encoded_all_key(row, schema);
    if (!all_key.ok())
        return all_key.status();
    return add_all_key(*all_key);
}

absl::Status Builder::add_all_key(const types::EncodedAllKey& all_key) {
    hashes_.push_back(hash_key(all_key.bytes()));
    return absl::OkStatus();
}

std::string Builder::finish() const {
    const uint64_t min_bits = 64;
    const uint64_t bit_count = std::max<uint64_t>(
        min_bits, static_cast<uint64_t>(hashes_.size()) * static_cast<uint64_t>(bits_per_key_));
    const uint64_t byte_count = (bit_count + 7) / 8;
    std::string bits(static_cast<size_t>(byte_count), '\0');

    const int probes = pl::StandardBloomFilter::choose_num_probes(bits_per_key_);
    for (uint32_t h : hashes_) {
        pl::StandardBloomFilter::add_hash(
            h, static_cast<uint32_t>(byte_count * 8), probes, bits.data());
    }

    Header h;
    h.hash_count = static_cast<uint32_t>(probes);
    h.bit_count = byte_count * 8;
    h.row_count = hashes_.size();

    std::string section;
    encode_header(h, &section);
    section.append(bits);
    h.checksum = codec::crc32c_u64(section);

    section.clear();
    encode_header(h, &section);
    section.append(bits);
    return section;
}

absl::StatusOr<Reader> Reader::open(std::string_view section) {
    if (section.size() < Header::kSize) {
        return absl::InvalidArgumentError("bloom section is shorter than header");
    }
    Header h = decode_header(section);
    if (h.magic != Header::kMagic) {
        return absl::InvalidArgumentError("bloom magic mismatch");
    }
    if (h.version != Header::kVersion) {
        return absl::InvalidArgumentError("unsupported bloom version");
    }
    const size_t bytes = static_cast<size_t>((h.bit_count + 7) / 8);
    if (section.size() != Header::kSize + bytes) {
        return absl::InvalidArgumentError("bloom section length mismatch");
    }

    Header zero = h;
    zero.checksum = 0;
    std::string checksum_input;
    encode_header(zero, &checksum_input);
    checksum_input.append(section.substr(Header::kSize));
    if (codec::crc32c_u64(checksum_input) != h.checksum) {
        return absl::InvalidArgumentError("bloom checksum mismatch");
    }

    Reader reader;
    reader.header_ = h;
    reader.bits_.assign(section.substr(Header::kSize));
    return reader;
}

bool Reader::may_contain_all_key(const types::EncodedAllKey& all_key) const {
    if (bits_.empty() || header_.hash_count == 0)
        return false;
    return pl::StandardBloomFilter::hash_may_match(hash_key(all_key.bytes()),
                                                   static_cast<uint32_t>(header_.bit_count),
                                                   static_cast<int>(header_.hash_count),
                                                   bits_.data());
}

absl::StatusOr<bool> Reader::may_contain(const types::InternalRow& row,
                                         types::InternalSchema::ConstRef schema) const {
    auto all_key = codec::make_encoded_all_key(row, schema);
    if (!all_key.ok())
        return all_key.status();
    return may_contain_all_key(*all_key);
}

} // namespace pl::sstv2::bloom
