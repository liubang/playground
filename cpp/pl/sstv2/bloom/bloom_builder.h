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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pl::sstv2::bloom {

// Builds a blocked bloom filter using cache-line-aligned blocks.
// Internally delegates to pl::BlockedBloomFilter for bit manipulation.
class BloomBuilder {
public:
    explicit BloomBuilder(size_t expected_keys, size_t bits_per_key = 10);

    void add_key(std::string_view key);

    // Returns complete bloom filter data (header + bit array)
    std::string finish();

    size_t num_keys_added() const { return num_keys_added_; }

private:
    int num_probes_;
    uint32_t millibits_per_key_;
    uint32_t bytes_len_; // bit array size in bytes (64-byte aligned)
    size_t num_keys_added_ = 0;
    std::vector<char> data_; // bit array
};

} // namespace pl::sstv2::bloom
