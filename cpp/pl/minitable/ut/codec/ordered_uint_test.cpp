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
// Created: 2026/07/17 22:27

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>

#include "cpp/pl/sstv2/codec/ordered_uint.h"

namespace pl::sstv2::codec {
namespace {

TEST(OrderedUintTest, GoldenAndRoundTrip) {
    struct Case {
        uint32_t value;
        std::string encoded;
    };
    const std::array cases{
        Case{0, std::string("\x00", 1)},
        Case{1, std::string("\x01\x01", 2)},
        Case{255, std::string("\x01\xff", 2)},
        Case{256, std::string("\x02\x01\x00", 3)},
        Case{65536, std::string("\x03\x01\x00\x00", 4)},
        Case{std::numeric_limits<uint32_t>::max(), std::string("\x04\xff\xff\xff\xff", 5)},
    };
    for (const auto& test : cases) {
        std::string encoded;
        encode_ordered_uint32(test.value, &encoded);
        EXPECT_EQ(encoded, test.encoded);
        uint32_t decoded = 0;
        EXPECT_EQ(decode_ordered_uint32(
                      reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(), &decoded),
                  encoded.size());
        EXPECT_EQ(decoded, test.value);
    }
}

TEST(OrderedUintTest, PreservesNumericOrder) {
    const std::array<uint32_t, 10> values{
        0, 1, 127, 128, 255, 256, 65535, 65536, 16777216, std::numeric_limits<uint32_t>::max()};
    std::string previous;
    for (uint32_t value : values) {
        std::string encoded;
        encode_ordered_uint32(value, &encoded);
        if (!previous.empty()) {
            EXPECT_LT(previous, encoded);
        }
        previous = std::move(encoded);
    }
}

TEST(OrderedUintTest, RejectsMalformedAndNonCanonicalInput) {
    uint32_t value = 0;
    const uint8_t invalid_width[]{5, 1, 2, 3, 4, 5};
    EXPECT_EQ(decode_ordered_uint32(invalid_width, sizeof(invalid_width), &value), 0U);

    const uint8_t non_canonical[]{2, 0, 1};
    EXPECT_EQ(decode_ordered_uint32(non_canonical, sizeof(non_canonical), &value), 0U);

    const uint8_t truncated[]{4, 1, 2};
    EXPECT_EQ(decode_ordered_uint32(truncated, sizeof(truncated), &value), 0U);
}

} // namespace
} // namespace pl::sstv2::codec
