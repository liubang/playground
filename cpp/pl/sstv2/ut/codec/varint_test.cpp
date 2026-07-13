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

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

#include "cpp/pl/sstv2/codec/varint.h"

namespace pl::sstv2::codec {
namespace {

TEST(VarintTest, RoundTrip) {
    uint64_t test_values[] = {
        0,
        1,
        127,
        128,
        255,
        256,
        16383,
        16384,
        UINT32_MAX,
        UINT64_MAX,
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

TEST(VarintTest, DecodeEmptyBuffer) {
    uint64_t decoded = 42;
    EXPECT_EQ(decode_varint(nullptr, 0, &decoded), 0u);
    EXPECT_EQ(decoded, 42u); // value unchanged on failure
}

TEST(VarintTest, DecodeRejectsNullPointers) {
    const uint8_t byte = 1;
    uint64_t decoded = 42;
    EXPECT_EQ(decode_varint(nullptr, 1, &decoded), 0u);
    EXPECT_EQ(decoded, 42u);
    EXPECT_EQ(decode_varint(&byte, 1, nullptr), 0u);
}

TEST(VarintTest, DecodeOverflow10thByte) {
    // A valid 10-byte varint for UINT64_MAX: 9 bytes of 0xFF + final byte 0x01.
    uint8_t valid_max[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    uint64_t decoded = 0;
    EXPECT_EQ(decode_varint(valid_max, 10, &decoded), 10u);
    EXPECT_EQ(decoded, UINT64_MAX);

    // 10th byte = 0x02 means bit 1 is set, which would require bit 64 — overflow.
    uint8_t overflow1[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02};
    EXPECT_EQ(decode_varint(overflow1, 10, &decoded), 0u);

    // 10th byte = 0x7F — all payload bits set, clearly overflow.
    uint8_t overflow2[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    EXPECT_EQ(decode_varint(overflow2, 10, &decoded), 0u);

    // 10th byte = 0x03 with continuation bit clear — still overflow (bits 1-6 nonzero).
    uint8_t overflow3[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03};
    EXPECT_EQ(decode_varint(overflow3, 10, &decoded), 0u);
}

TEST(VarintTest, DecodeNeverEnding) {
    // 10 bytes all with continuation bit set — no terminator within limit.
    uint8_t buf[10] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
    uint64_t decoded = 0;
    EXPECT_EQ(decode_varint(buf, 10, &decoded), 0u);
}

TEST(VarintTest, DecodeIgnoresTrailingBytes) {
    // Encode a 2-byte varint, provide extra trailing bytes, verify only 2 consumed.
    uint8_t buf[10];
    size_t written = encode_varint(300, buf);
    ASSERT_EQ(written, 2u);
    buf[2] = 0xFF; // trailing garbage

    uint64_t decoded = 0;
    EXPECT_EQ(decode_varint(buf, 10, &decoded), 2u);
    EXPECT_EQ(decoded, 300u);
}

TEST(ZigZagTest, RoundTrip) {
    int64_t test_values[] = {
        0,
        1,
        -1,
        2,
        -2,
        127,
        -128,
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
