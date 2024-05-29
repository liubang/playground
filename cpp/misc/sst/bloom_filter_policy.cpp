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

#include "cpp/misc/bloom/bloom_filter.h"
#include "cpp/misc/sst/filter_policy.h"

namespace pl {

/**
 * @class BloomFilterPolicy
 * @brief this class is a proxy of bloom::BloomFilter
 */
class BloomFilterPolicy : public FilterPolicy {
public:
    BloomFilterPolicy(std::size_t bits_per_key) : bits_per_key_(bits_per_key) {}

    ~BloomFilterPolicy() override = default;

    void createFilter(Binary* keys, std::size_t n, std::string* dst) const override {
        BloomFilter bloom_filter(bits_per_key_);
        bloom_filter.create(keys, n, dst);
    }

    [[nodiscard]] const char* name() const override { return "BloomFilterPolicy"; }

    [[nodiscard]] bool keyMayMatch(const Binary& key, const Binary& filter) const override {
        BloomFilter bloom_filter(bits_per_key_);
        return bloom_filter.contains(key, filter);
    }

private:
    std::size_t bits_per_key_;
};

FilterPolicy* newBloomFilterPolicy(uint64_t bits_per_key) {
    return new BloomFilterPolicy(bits_per_key);
}

} // namespace pl
