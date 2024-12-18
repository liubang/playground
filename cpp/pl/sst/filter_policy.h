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

#include "xxhash.h"
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pl {

class FilterBuilder;
class FilterReader;

class FilterBuilder {
public:
    virtual ~FilterBuilder();

    virtual void add_key(std::string_view key) = 0;

    virtual std::string_view finish(std::unique_ptr<const char[]>* buf) = 0;
};

class BlockedBloomFilterBuilder : public FilterBuilder {
    constexpr static int kMetadataLen = 5;

public:
    BlockedBloomFilterBuilder(int millibits_per_key) : millibits_per_key_(millibits_per_key) {}

    void add_key(std::string_view key) {
        uint64_t hash = XXH3_64bits(key.data(), key.size());
        if (hashes_.empty() || hashes_.back() != hash) {
            hashes_.push_back(hash);
            xor_checksum_ ^= hash;
        }
    }

    std::string_view finish(std::unique_ptr<const char[]>* buf) {
        std::size_t num_keys = hashes_.size();
        if (num_keys == 0) {
            return {};
        }
        return {};
    }

private:
    std::size_t calculate_space(std::size_t num_hashes) {
        size_t raw_target_len =
            static_cast<size_t>((uint64_t{num_hashes} * millibits_per_key_ + 7999) / 8000);

        if (raw_target_len >= size_t{0xffffffc0}) {
            raw_target_len = size_t{0xffffffc0};
        }

        return ((raw_target_len + 63) & ~size_t{63}) + kMetadataLen;
        return 0;
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
