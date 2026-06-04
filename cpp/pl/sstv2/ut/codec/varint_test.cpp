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
// Created: 2026/06/05 00:23

#include "cpp/pl/sstv2/codec/varint.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace pl::sstv2::codec {
namespace {

TEST(VarintTest, RoundTrip) {
    uint64_t test_values[] = {
        0, 1, 127, 128, 255, 256, 16383, 16384,
        UINT32_MAX, UINT64_MAX,
        (uint64_t{1} << 7) - 1,
        (uint64_t{1} << 14) - 1,
        (uint64_t{1} << 21) - 1,
        (uint64_t{1} << 28) - 1,
        (uint64_t{1} << 35) - 1,
        (uint64_t{1} << 42) - 1,
        (uint64_t{1} << 49) - 1,
        (uint64_t{1} << 56) - 1,
        (uint64_t{1} << 63) - 1,
    };

    uint8_t buf[10];
    for (uint64_t expected : test_values) {
        size_t written = encode_varint(expected, buf);
        ASSERT_GT(written, 0u);
        ASSERT_LE(written, 10u);

        uint64_t decoded = 0;
        size_t consumed = decode_varint(buf, written, &decoded);
        ASSERT_EQ(consumed, written);
        ASSERT_EQ(decoded, expected);
    }
}

TEST(VarintTest, Length) {
    EXPECT_EQ(varint_length(0), 1u);
    EXPECT_EQ(varint_length(127), 1u);
    EXPECT_EQ(varint_length(128), 2u);
    EXPECT_EQ(varint_length(16383), 2u);
    EXPECT_EQ(varint_length(16384), 3u);
    EXPECT_EQ(varint_length(UINT64_MAX), 10u);
}

TEST(VarintTest, DecodeTruncated) {
    // Encode a multi-byte varint, then try decoding with insufficient bytes.
    uint8_t buf[10];
    size_t written = encode_varint(300, buf); // 300 requires 2 bytes
    ASSERT_EQ(written, 2u);

    uint64_t decoded = 0;
    EXPECT_EQ(decode_varint(buf, 1, &decoded), 0u); // truncated
}

TEST(ZigZagTest, RoundTrip) {
    int64_t test_values[] = {
        0, 1, -1, 2, -2, 127, -128,
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::min(),
    };

    for (int64_t expected : test_values) {
        uint64_t encoded = zigzag_encode(expected);
        int64_t decoded = zigzag_decode(encoded);
        ASSERT_EQ(decoded, expected);
    }
}

TEST(ZigZagTest, SmallAbsoluteValuesAreSmall) {
    // ZigZag maps small absolute values to small unsigned values.
    EXPECT_EQ(zigzag_encode(0), 0u);
    EXPECT_EQ(zigzag_encode(-1), 1u);
    EXPECT_EQ(zigzag_encode(1), 2u);
    EXPECT_EQ(zigzag_encode(-2), 3u);
    EXPECT_EQ(zigzag_encode(2), 4u);
}

} // namespace
} // namespace pl::sstv2::codec
