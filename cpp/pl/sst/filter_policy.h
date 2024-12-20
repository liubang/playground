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

#pragma once

#include "cpp/pl/bloom/bloom.h"

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

#include "xxhash.h"

namespace pl {

class FilterBuilder {
public:
    virtual ~FilterBuilder();

    virtual void add_key(std::string_view key) = 0;

    virtual std::string_view finish(std::unique_ptr<const char[]>* buf) = 0;
};

inline uint32_t Lower32of64(uint64_t v) { return static_cast<uint32_t>(v); }
inline uint32_t Upper32of64(uint64_t v) { return static_cast<uint32_t>(v >> 32); }

class BlockedBloomFilterBuilder : public FilterBuilder {
    constexpr static int kMetadataLen = 5; // magic_code + num_probes
    constexpr static std::size_t kBufferMask = 7;
    constexpr static uint32_t kMagicCode = 0x4D4F4C42;

public:
    BlockedBloomFilterBuilder(int millibits_per_key) : millibits_per_key_(millibits_per_key) {}

    ~BlockedBloomFilterBuilder() override = default;

    void add_key(std::string_view key) override {
        uint64_t hash = XXH3_64bits(key.data(), key.size());
        if (hashes_.empty() || hashes_.back() != hash) {
            hashes_.push_back(hash);
            // TODO: to use xor_checksum
            xor_checksum_ ^= hash;
        }
    }

    std::string_view finish(std::unique_ptr<const char[]>* buf) override {
        std::size_t num_hashes = hashes_.size();
        if (num_hashes == 0) {
            return {};
        }
        std::size_t length_with_metadata = calculate_space(num_hashes);
        std::size_t length = length_with_metadata - kMetadataLen;
        std::unique_ptr<char[]> mutable_buf = std::make_unique<char[]>(length_with_metadata);
        int num_probes = get_num_probes(num_hashes, length_with_metadata);
        add_all_hashes(mutable_buf.get(), length, num_probes);

        memcpy(mutable_buf.get() + length, &kMagicCode, sizeof(uint32_t));
        mutable_buf[length + sizeof(uint32_t)] = static_cast<char>(num_probes);

        std::string_view result(mutable_buf.get(), length_with_metadata);
        *buf = std::move(mutable_buf);
        return result;
    }

private:
    void add_all_hashes(char* buf, std::size_t buf_len, int num_probes) {
        std::size_t num_hashes = hashes_.size();
        std::array<uint32_t, kBufferMask + 1> hashes;
        std::array<uint32_t, kBufferMask + 1> byte_offsets;

        std::size_t i = 0;
        std::deque<uint64_t>::iterator hash_entries_it = hashes_.begin();
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

    int get_num_probes(std::size_t num_hashes, std::size_t length_with_metadata) {
        uint64_t millibits = uint64_t{length_with_metadata - kMetadataLen} * 8000;
        int actual_millibits_per_key =
            static_cast<int>(millibits / std::max(num_hashes, std::size_t{1}));
        return BlockedBloomFilter::choose_num_probes(actual_millibits_per_key);
    }

    std::size_t calculate_space(std::size_t num_hashes) {
        std::size_t raw_target_len =
            static_cast<std::size_t>((uint64_t{num_hashes} * millibits_per_key_ + 7999) / 8000);

        if (raw_target_len >= std::size_t{0xffffffc0}) {
            raw_target_len = std::size_t{0xffffffc0};
        }

        return ((raw_target_len + 63) & ~size_t{63}) + kMetadataLen;
    }

private:
    std::deque<uint64_t> hashes_;
    uint64_t xor_checksum_{0};
    int millibits_per_key_{0};
};

class FilterReader {
public:
    virtual ~FilterReader() = 0;

    virtual bool key_may_match(std::string_view key) = 0;
};

class BlockedBloomFilterReader : public FilterReader {
public:
    BlockedBloomFilterReader(const char* buf, uint32_t buf_length, int num_probes)
        : buf_(buf), buf_length_(buf_length), num_probes_(num_probes) {}

    ~BlockedBloomFilterReader() override = default;

    bool key_may_match(std::string_view key) override {
        uint64_t hash = XXH3_64bits(key.data(), key.size());
        uint32_t byte_offset;
        BlockedBloomFilter::prepare_hash(Lower32of64(hash), buf_length_, buf_, &byte_offset);
        return BlockedBloomFilter::hash_may_match_prepared(Upper32of64(hash), num_probes_,
                                                           buf_ + byte_offset);
    }

private:
    const char* buf_{nullptr};
    const uint32_t buf_length_{0};
    const int num_probes_{0};
};

class FilterPolicy;

using FilterPolicyPtr = std::unique_ptr<FilterPolicy>;
using FilterPolicyRef = std::shared_ptr<FilterPolicy>;

class FilterPolicy {
public:
    virtual ~FilterPolicy() = default;

    /**
     * @brief description]
     *
     * @param keys parameter]
     * @param n parameter]
     * @param dst parameter]
     */
    virtual void createFilter(const std::vector<std::string_view>& keys,
                              std::string* dst) const = 0;

    /**
     * @brief description]
     *
     * @return return]
     */
    [[nodiscard]] virtual const char* name() const = 0;

    /**
     * @brief description]
     *
     * @param key parameter]
     * @param filter parameter]
     * @return return]
     */
    [[nodiscard]] virtual bool keyMayMatch(std::string_view key, std::string_view filter) const = 0;
};

/**
 * @class BloomFilterPolicy
 * @brief this class is a proxy of bloom::BloomFilter
 */
class BloomFilterPolicy : public FilterPolicy {
public:
    BloomFilterPolicy(uint32_t bits_per_key) : bits_per_key_(bits_per_key) {}

    ~BloomFilterPolicy() override = default;

    [[nodiscard]] const char* name() const override { return "BloomFilterPolicy"; }

    void createFilter(const std::vector<std::string_view>& keys, std::string* dst) const override;

    [[nodiscard]] bool keyMayMatch(std::string_view key, std::string_view filter) const override;

private:
    uint32_t bits_per_key_;
};

} // namespace pl
