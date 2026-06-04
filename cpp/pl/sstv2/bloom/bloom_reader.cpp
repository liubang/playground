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

#include "cpp/pl/sstv2/bloom/bloom_reader.h"

#include <cmath>

#include "cpp/pl/bloom/bloom.h"

namespace pl::sstv2::bloom {

namespace {

void hash_key(std::string_view key, uint32_t* h1, uint32_t* h2) {
    uint32_t hash = 0;
    for (size_t i = 0; i < key.size(); ++i) {
        hash = hash * 31 + static_cast<uint8_t>(key[i]);
    }
    *h1 = hash;
    *h2 = (hash >> 17) | (hash << 15);
    *h2 = *h2 * 0x9e3779b9 + 0xdeadbeef;
    for (size_t i = 0; i < key.size(); ++i) {
        *h2 = *h2 ^ (static_cast<uint8_t>(key[i]) * 0x5bd1e995);
        *h2 = (*h2 >> 13) ^ *h2;
    }
}

} // namespace

absl::StatusOr<BloomReader> BloomReader::open(std::span<const std::byte> data) {
    if (data.size() < BloomHeader::kSize) {
        return absl::InvalidArgumentError("bloom filter data too small for header");
    }

    BloomReader reader;
    reader.header_ = BloomHeader::decode(data.data());

    reader.bytes_len_ = reader.header_.num_bits / 8;
    if (data.size() < BloomHeader::kSize + reader.bytes_len_) {
        return absl::InvalidArgumentError("bloom filter data truncated");
    }

    reader.bits_ = reinterpret_cast<const char*>(data.data() + BloomHeader::kSize);
    return reader;
}

bool BloomReader::may_contain(std::string_view key) const {
    uint32_t h1, h2;
    hash_key(key, &h1, &h2);
    return ::pl::BlockedBloomFilter::hash_may_match(
        h1, h2, bytes_len_, static_cast<int>(header_.num_probes), bits_);
}

double BloomReader::false_positive_rate() const {
    if (header_.num_bits == 0) {
        return 1.0;
    }
    double bits_per_key = static_cast<double>(header_.millibits_per_key) / 1000.0;
    return ::pl::BloomMath::blocked_bloom_fp_rate(
        bits_per_key, static_cast<int>(header_.num_probes), 512);
}

} // namespace pl::sstv2::bloom
