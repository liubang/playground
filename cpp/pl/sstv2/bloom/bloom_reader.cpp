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
#include <cstring>

#include "cpp/pl/bloom/bloom.h"

namespace pl::sstv2::bloom {

namespace {

inline uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

void hash_key(std::string_view key, uint32_t* h1, uint32_t* h2) {
    const auto* data = reinterpret_cast<const uint8_t*>(key.data());
    size_t len = key.size();

    uint32_t seed1 = 0xcc9e2d51;
    uint32_t seed2 = 0x1b873593;

    uint32_t ha = seed1;
    uint32_t hb = seed2;

    size_t nblocks = len / 4;
    for (size_t i = 0; i < nblocks; ++i) {
        uint32_t k;
        std::memcpy(&k, data + i * 4, 4);

        k *= 0xcc9e2d51;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593;
        ha ^= k;
        ha = (ha << 13) | (ha >> 19);
        ha = ha * 5 + 0xe6546b64;

        k *= 0x85ebca6b;
        k = (k << 13) | (k >> 19);
        k *= 0xc2b2ae35;
        hb ^= k;
        hb = (hb << 16) | (hb >> 16);
        hb = hb * 3 + 0x27d4eb2f;
    }

    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3:
            k1 ^= static_cast<uint32_t>(tail[2]) << 16;
            [[fallthrough]];
        case 2:
            k1 ^= static_cast<uint32_t>(tail[1]) << 8;
            [[fallthrough]];
        case 1:
            k1 ^= static_cast<uint32_t>(tail[0]);
            k1 *= 0xcc9e2d51;
            k1 = (k1 << 15) | (k1 >> 17);
            k1 *= 0x1b873593;
            ha ^= k1;
            hb ^= k1;
    }

    ha ^= static_cast<uint32_t>(len);
    hb ^= static_cast<uint32_t>(len);

    *h1 = fmix32(ha);
    *h2 = fmix32(hb);
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
