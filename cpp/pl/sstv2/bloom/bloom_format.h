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

#include "cpp/pl/sstv2/encode/fixed.h"

namespace pl::sstv2::bloom {

// On-disk bloom filter format (blocked bloom filter, cache-line aligned):
//
// ┌───────────────────────────────┐
// │      Bloom Filter Header      │
// │  - num_keys (uint32)          │
// │  - num_bits (uint32)          │  (= bytes_len * 8)
// │  - num_probes (uint8)         │
// │  - millibits_per_key (uint32) │
// ├───────────────────────────────┤
// │  Bit Array Data (64B aligned) │
// └───────────────────────────────┘

struct BloomHeader {
    static constexpr size_t kSize = 13; // 4+4+1+4

    uint32_t num_keys = 0;
    uint32_t num_bits = 0;          // total bits in the bit array
    uint8_t num_probes = 0;         // number of hash probes
    uint32_t millibits_per_key = 0; // bits_per_key * 1000

    void encode(std::byte* dst) const {
        using pl::sstv2::encode::encode_fixed;
        encode_fixed<uint32_t>(num_keys, dst);
        encode_fixed<uint32_t>(num_bits, dst + 4);
        dst[8] = static_cast<std::byte>(num_probes);
        encode_fixed<uint32_t>(millibits_per_key, dst + 9);
    }

    static BloomHeader decode(const std::byte* src) {
        using pl::sstv2::encode::decode_fixed;
        BloomHeader h;
        h.num_keys = decode_fixed<uint32_t>(src);
        h.num_bits = decode_fixed<uint32_t>(src + 4);
        h.num_probes = static_cast<uint8_t>(src[8]);
        h.millibits_per_key = decode_fixed<uint32_t>(src + 9);
        return h;
    }
};

} // namespace pl::sstv2::bloom
