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

// MurmurHash-like hash for bloom filter keys
// Splits a key into two 32-bit hashes for BlockedBloomFilter
void hash_key(std::string_view key, uint32_t* h1, uint32_t* h2) {
    // Use a simple but effective hash: first half from one seed, second from another
    uint32_t hash = 0;
    for (size_t i = 0; i < key.size(); ++i) {
        hash = hash * 31 + static_cast<uint8_t>(key[i]);
    }
    *h1 = hash;
    // Rotate and mix for h2
    *h2 = (hash >> 17) | (hash << 15);
    *h2 = *h2 * 0x9e3779b9 + 0xdeadbeef;
    for (size_t i = 0; i < key.size(); ++i) {
        *h2 = *h2 ^ (static_cast<uint8_t>(key[i]) * 0x5bd1e995);
        *h2 = (*h2 >> 13) ^ *h2;
    }
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
