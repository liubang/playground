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

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace pl {

class BloomFilter final {
public:
    explicit BloomFilter(std::size_t bit_per_key);
    BloomFilter(const BloomFilter&) = delete;
    BloomFilter(const BloomFilter&&) = delete;
    BloomFilter& operator=(const BloomFilter&) = delete;
    BloomFilter& operator=(const BloomFilter&&) = delete;

    // lookup if bloomfilter contains data
    [[nodiscard]] bool contains(std::string_view key, std::string_view filter) const;

    // add a member to bloomfilter
    void create(const std::vector<std::string_view>& keys, std::string* dst) const;

private:
    std::size_t bits_per_key_; // 每一个key需要占用多少bit
    std::size_t hash_count_;   // hash函数的个数
};

} // namespace pl
