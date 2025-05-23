// Copyright (c) 2024 The Authors. All rights reserved.
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

#include "cpp/pl/sst/filter_policy.h"

#include "xxhash.h"
#include <array>

namespace pl {

static constexpr uint32_t kMetadataLen = 5;   // version(4B)+ num_probes(1B)
static constexpr uint32_t kBloomFilterV1 = 1; // Standard BloomFilter
static constexpr uint32_t kBloomFilterV2 = 2; // Blocked BloomFilter

namespace {
inline uint32_t Lower32of64(uint64_t v) { return static_cast<uint32_t>(v); }
inline uint32_t Upper32of64(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
} // namespace

void FilterBuilder::add_key(std::string_view key) {
    uint64_t hash = ::XXH3_64bits(key.data(), key.size());
    if (hashes_.empty() || hashes_.back() != hash) {
        hashes_.push_back(hash);
    }
}

void FilterBuilder::add_key_and_alt(std::string_view key, std::string_view alt) {
    uint64_t key_hash = ::XXH3_64bits(key.data(), key.size());
    uint64_t alt_hash = ::XXH3_64bits(alt.data(), alt.size());

    std::optional<uint64_t> prev_key_hash;
    std::optional<uint64_t> prev_alt_hash = prev_alt_hash_;
    if (!hashes_.empty()) {
        prev_key_hash = hashes_.back();
    }

    if (alt_hash != prev_alt_hash && alt_hash != key_hash && alt_hash != prev_key_hash) {
        hashes_.push_back(alt_hash);
    }
    prev_alt_hash_ = alt_hash;
    if (key_hash != prev_key_hash && key_hash != prev_alt_hash) {
        hashes_.push_back(key_hash);
    }
}

size_t FilterBuilder::estimate_hashes_added() { return hashes_.size(); }

std::size_t StandardBloomFilterBuilder::calculate_space(std::size_t num_hashes) const {
    std::size_t bit_count = num_hashes * bits_per_key_;
    std::size_t actual_bit_count = 8;
    while (actual_bit_count < bit_count) {
        actual_bit_count <<= 1;
    }
    return (actual_bit_count >> 3) + kMetadataLen;
}

std::string_view StandardBloomFilterBuilder::finish(std::unique_ptr<const char[]>* buf) {
    std::size_t num_hashes = hashes_.size();
    if (num_hashes == 0) {
        return {};
    }
    std::size_t length_with_metadata = calculate_space(num_hashes);
    std::size_t length = length_with_metadata - kMetadataLen;
    std::unique_ptr<char[]> mutable_buf = std::make_unique<char[]>(length_with_metadata);
    int num_probes = StandardBloomFilter::choose_num_probes(bits_per_key_);
    add_all_hashes(mutable_buf.get(), length, num_probes);

    memcpy(mutable_buf.get() + length, &kBloomFilterV1, sizeof(uint32_t));
    mutable_buf[length + sizeof(uint32_t)] = static_cast<char>(num_probes);

    std::string_view result(mutable_buf.get(), length_with_metadata);
    *buf = std::move(mutable_buf);
    return result;
}

void StandardBloomFilterBuilder::add_all_hashes(char* buf, std::size_t buf_len, int num_probes) {
    std::size_t total_bits = buf_len << 3;
    for (auto hash : hashes_) {
        StandardBloomFilter::add_hash(hash, total_bits, num_probes, buf);
    }
}

std::string_view BlockedBloomFilterBuilder::finish(std::unique_ptr<const char[]>* buf) {
    std::size_t num_hashes = hashes_.size();
    if (num_hashes == 0) {
        return {};
    }
    std::size_t length_with_metadata = calculate_space(num_hashes);
    std::size_t length = length_with_metadata - kMetadataLen;
    std::unique_ptr<char[]> mutable_buf = std::make_unique<char[]>(length_with_metadata);
    int num_probes = get_num_probes(num_hashes, length_with_metadata);
    add_all_hashes(mutable_buf.get(), length, num_probes);

    memcpy(mutable_buf.get() + length, &kBloomFilterV2, sizeof(uint32_t));
    mutable_buf[length + sizeof(uint32_t)] = static_cast<char>(num_probes);

    std::string_view result(mutable_buf.get(), length_with_metadata);
    *buf = std::move(mutable_buf);
    return result;
}

void BlockedBloomFilterBuilder::add_all_hashes(char* buf, std::size_t buf_len, int num_probes) {
    constexpr std::size_t kBufferMask = 7;
    std::size_t num_hashes = hashes_.size();
    std::array<uint32_t, kBufferMask + 1> hashes;
    std::array<uint32_t, kBufferMask + 1> byte_offsets;

    std::size_t i = 0;
    auto hash_entries_it = hashes_.begin();
    for (; i <= kBufferMask && i < num_hashes; ++i) {
        uint64_t h = *hash_entries_it;
        BlockedBloomFilter::prepare_hash(Lower32of64(h), buf_len, buf, &byte_offsets[i]);
        hashes[i] = Upper32of64(h);
        ++hash_entries_it;
    }

    for (; i < num_hashes; ++i) {
        uint32_t& hash_ref = hashes[i & kBufferMask];
        uint32_t& byte_offset_ref = byte_offsets[i & kBufferMask];
        BlockedBloomFilter::add_hash_prepared(hash_ref, num_probes, buf + byte_offset_ref);
        uint64_t h = *hash_entries_it;
        BlockedBloomFilter::prepare_hash(Lower32of64(h), buf_len, buf, &byte_offset_ref);
        hash_ref = Upper32of64(h);
        ++hash_entries_it;
    }

    // Finish processing
    for (i = 0; i <= kBufferMask && i < num_hashes; ++i) {
        BlockedBloomFilter::add_hash_prepared(hashes[i], num_probes, buf + byte_offsets[i]);
    }
}

int BlockedBloomFilterBuilder::get_num_probes(std::size_t num_hashes,
                                              std::size_t length_with_metadata) {
    uint64_t millibits = uint64_t{length_with_metadata - kMetadataLen} * 8000;
    int actual_millibits_per_key =
        static_cast<int>(millibits / std::max(num_hashes, std::size_t{1}));
    return BlockedBloomFilter::choose_num_probes(actual_millibits_per_key);
}

std::size_t BlockedBloomFilterBuilder::calculate_space(std::size_t num_hashes) const {
    auto raw_target_len =
        static_cast<std::size_t>((uint64_t{num_hashes} * millibits_per_key_ + 7999) / 8000);
    if (raw_target_len >= std::size_t{0xffffffc0}) {
        raw_target_len = std::size_t{0xffffffc0};
    }
    return ((raw_target_len + 63) & ~size_t{63}) + kMetadataLen;
}

bool StandardBloomFilterReader::key_may_match(std::string_view key) {
    uint64_t hash = ::XXH3_64bits(key.data(), key.size());
    return StandardBloomFilter::hash_may_match(hash, (buf_length_ - kMetadataLen) << 3, num_probes_,
                                               buf_);
}

bool BlockedBloomFilterReader::key_may_match(std::string_view key) {
    uint64_t hash = ::XXH3_64bits(key.data(), key.size());
    uint32_t byte_offset;
    BlockedBloomFilter::prepare_hash(Lower32of64(hash), buf_length_ - kMetadataLen, buf_,
                                     &byte_offset);
    return BlockedBloomFilter::hash_may_match_prepared(Upper32of64(hash), num_probes_,
                                                       buf_ + byte_offset);
}

} // namespace pl
