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
// Created: 2026/06/04 12:01

#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>

#include "cpp/pl/sstv2/encode/varints.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::encode;

// === uint32 encode/decode round-trip ===

TEST(VarintsTest, Uint32RoundTrip) {
    uint32_t values[] = {0, 1, 127, 128, 16383, 16384, UINT32_MAX};

    for (uint32_t val : values) {
        std::byte buf[Varints::kMaxVarint32Bytes];
        size_t written = Varints::encode_uint32(val, buf);
        ASSERT_GT(written, 0u);
        ASSERT_LE(written, Varints::kMaxVarint32Bytes);

        auto [decoded, consumed] = Varints::decode_uint32(std::span<const std::byte>(buf, written));
        EXPECT_EQ(decoded, val);
        EXPECT_EQ(consumed, written);
    }
}

// === uint64 encode/decode round-trip ===

TEST(VarintsTest, Uint64RoundTrip) {
    uint64_t values[] = {0,
                         1,
                         127,
                         128,
                         16383,
                         16384,
                         (1ULL << 21) - 1,
                         1ULL << 21,
                         (1ULL << 35) - 1,
                         1ULL << 49,
                         UINT64_MAX};

    for (uint64_t val : values) {
        std::byte buf[Varints::kMaxVarint64Bytes];
        size_t written = Varints::encode_uint64(val, buf);
        ASSERT_GT(written, 0u);
        ASSERT_LE(written, Varints::kMaxVarint64Bytes);

        auto [decoded, consumed] = Varints::decode_uint64(std::span<const std::byte>(buf, written));
        EXPECT_EQ(decoded, val);
        EXPECT_EQ(consumed, written);
    }
}

// === ZigZag encode/decode ===

TEST(VarintsTest, ZigZag32) {
    // Known mappings: 0→0, -1→1, 1→2, -2→3, 2→4
    EXPECT_EQ(Varints::zigzag_encode32(0), 0u);
    EXPECT_EQ(Varints::zigzag_encode32(-1), 1u);
    EXPECT_EQ(Varints::zigzag_encode32(1), 2u);
    EXPECT_EQ(Varints::zigzag_encode32(-2), 3u);
    EXPECT_EQ(Varints::zigzag_encode32(2), 4u);

    // Round-trip
    int32_t values[] = {0, -1, 1, -2, 2, INT32_MIN, INT32_MAX};
    for (int32_t val : values) {
        uint32_t encoded = Varints::zigzag_encode32(val);
        int32_t decoded = Varints::zigzag_decode32(encoded);
        EXPECT_EQ(decoded, val);
    }
}

TEST(VarintsTest, ZigZag64) {
    // Known mappings
    EXPECT_EQ(Varints::zigzag_encode64(0), 0u);
    EXPECT_EQ(Varints::zigzag_encode64(-1), 1u);
    EXPECT_EQ(Varints::zigzag_encode64(1), 2u);

    // Round-trip
    int64_t values[] = {0, -1, 1, -2, 2, INT64_MIN, INT64_MAX};
    for (int64_t val : values) {
        uint64_t encoded = Varints::zigzag_encode64(val);
        int64_t decoded = Varints::zigzag_decode64(encoded);
        EXPECT_EQ(decoded, val);
    }
}

// === Verify encoded byte count ===

TEST(VarintsTest, EncodedByteCount32) {
    std::byte buf[Varints::kMaxVarint32Bytes];

    // Values 0-127 fit in 1 byte
    EXPECT_EQ(Varints::encode_uint32(0, buf), 1u);
    EXPECT_EQ(Varints::encode_uint32(127, buf), 1u);

    // 128 requires 2 bytes
    EXPECT_EQ(Varints::encode_uint32(128, buf), 2u);

    // 16383 (2^14 - 1) fits in 2 bytes
    EXPECT_EQ(Varints::encode_uint32(16383, buf), 2u);

    // 16384 (2^14) requires 3 bytes
    EXPECT_EQ(Varints::encode_uint32(16384, buf), 3u);

    // UINT32_MAX requires 5 bytes
    EXPECT_EQ(Varints::encode_uint32(UINT32_MAX, buf), 5u);
}

TEST(VarintsTest, EncodedByteCount64) {
    std::byte buf[Varints::kMaxVarint64Bytes];

    EXPECT_EQ(Varints::encode_uint64(0, buf), 1u);
    EXPECT_EQ(Varints::encode_uint64(127, buf), 1u);
    EXPECT_EQ(Varints::encode_uint64(128, buf), 2u);

    // UINT64_MAX requires 10 bytes
    EXPECT_EQ(Varints::encode_uint64(UINT64_MAX, buf), 10u);
}
