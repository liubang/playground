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

#include "cpp/pl/sstv2/bloom/bloom_builder.h"

#include <algorithm>
#include <cstring>

#include "cpp/pl/bloom/bloom.h"
#include "cpp/pl/sstv2/bloom/bloom_format.h"

namespace pl::sstv2::bloom {

namespace {

// MurmurHash3 32-bit finalizer for mixing
inline uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

// MurmurHash3-inspired hash producing two independent 32-bit values.
// This provides good distribution for the blocked bloom filter.
void hash_key(std::string_view key, uint32_t* h1, uint32_t* h2) {
    const auto* data = reinterpret_cast<const uint8_t*>(key.data());
    size_t len = key.size();

    uint32_t seed1 = 0xcc9e2d51;
    uint32_t seed2 = 0x1b873593;

    uint32_t ha = seed1;
    uint32_t hb = seed2;

    // Process 4 bytes at a time
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

    // Tail
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

BloomBuilder::BloomBuilder(size_t expected_keys, size_t bits_per_key) {
    millibits_per_key_ = static_cast<uint32_t>(bits_per_key * 1000);
    num_probes_ = ::pl::BlockedBloomFilter::choose_num_probes(static_cast<int>(millibits_per_key_));

    // Compute total bytes: cache-line (64B) aligned
    size_t total_bits = expected_keys * bits_per_key;
    size_t total_bytes = (total_bits + 7) / 8;
    // Round up to multiple of 64 (cache line)
    bytes_len_ = static_cast<uint32_t>((total_bytes + 63) & ~size_t{63});
    if (bytes_len_ < 64) {
        bytes_len_ = 64;
    }

    data_.resize(bytes_len_, 0);
}

void BloomBuilder::add_key(std::string_view key) {
    uint32_t h1, h2;
    hash_key(key, &h1, &h2);
    ::pl::BlockedBloomFilter::add_hash(h1, h2, bytes_len_, num_probes_, data_.data());
    ++num_keys_added_;
}

std::string BloomBuilder::finish() {
    std::string result;
    result.resize(BloomHeader::kSize + bytes_len_);

    BloomHeader header;
    header.num_keys = static_cast<uint32_t>(num_keys_added_);
    header.num_bits = bytes_len_ * 8;
    header.num_probes = static_cast<uint8_t>(num_probes_);
    header.millibits_per_key = millibits_per_key_;
    header.encode(reinterpret_cast<std::byte*>(result.data()));

    std::memcpy(result.data() + BloomHeader::kSize, data_.data(), bytes_len_);
    return result;
}

} // namespace pl::sstv2::bloom
