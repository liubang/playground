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
// Created: 2026/08/29 22:15

#include "cpp/pl/mllm/core/dtype.h"

#include <cstring>

namespace pl::mllm {

float fp16_to_fp32(uint16_t h) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(h & 0x8000U)) << 16;
    const uint32_t exp = (h >> 10) & 0x1FU;
    const uint32_t mant = h & 0x03FFU;

    uint32_t bits;
    if (exp == 0) { // subnormal or zero
        if (mant == 0) {
            bits = sign;
        } else {
            // normalize: mant = 0.mant * 2^-14
            uint32_t m = mant;
            int e = -1;
            while ((m & 0x0400U) == 0) {
                m <<= 1;
                --e;
            }
            m &= 0x03FFU;
            const uint32_t exp32 = static_cast<uint32_t>(127 - 15 + 1 + e);
            bits = sign | (exp32 << 23) | (m << 13);
        }
    } else if (exp == 0x1FU) { // inf / nan
        bits = sign | 0x7F800000U | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

uint16_t fp32_to_fp16(float f) noexcept {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));

    const uint32_t sign = (bits >> 16) & 0x8000U;
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFU) - 127 + 15;
    const uint32_t mant = bits & 0x007FFFFFU;

    if (exp <= 0) { // underflow to subnormal / zero
        if (exp < -10) {
            return static_cast<uint16_t>(sign);
        }
        const uint32_t m = mant | 0x00800000U;
        const uint32_t shifted = m >> static_cast<unsigned>(14 - exp);
        // round to nearest even
        const uint32_t rem = m & ((1U << static_cast<unsigned>(14 - exp)) - 1U);
        const uint32_t halfway = 1U << static_cast<unsigned>(13 - exp);
        uint32_t rounded =
            shifted + ((rem > halfway || (rem == halfway && (shifted & 1U)) ? 1U : 0U));
        return static_cast<uint16_t>(sign | rounded);
    }
    if (exp >= 31) {
        // NaN input stays NaN; finite overflow saturates to inf.
        if (((bits >> 23) & 0xFFU) == 0xFFU && mant != 0) {
            return static_cast<uint16_t>(sign | 0x7E00U);
        }
        return static_cast<uint16_t>(sign | 0x7C00U);
    }
    // round to nearest even on 13 dropped bits
    uint32_t half = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFU;
    if (rem > 0x1000U || (rem == 0x1000U && (half & 1U))) {
        ++half;
    }
    return static_cast<uint16_t>(sign | half);
}

} // namespace pl::mllm
