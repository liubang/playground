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
#include <span>
#include <string_view>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/bloom/bloom_format.h"

namespace pl::sstv2::bloom {

class BloomReader {
public:
    // Open from raw serialized data (header + bit array)
    static absl::StatusOr<BloomReader> open(std::span<const std::byte> data);

    // Returns true if key "may exist", false if "definitely not"
    bool may_contain(std::string_view key) const;

    uint32_t num_keys() const { return header_.num_keys; }
    uint32_t num_bits() const { return header_.num_bits; }
    double false_positive_rate() const;

private:
    BloomReader() = default;

    BloomHeader header_;
    const char* bits_ = nullptr;
    uint32_t bytes_len_ = 0;
};

} // namespace pl::sstv2::bloom
