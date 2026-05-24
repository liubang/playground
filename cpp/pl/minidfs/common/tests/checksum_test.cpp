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

#include <gtest/gtest.h>
#include <string>
#include <string_view>

#include "cpp/pl/minidfs/common/checksum.h"

namespace pl::minidfs {
namespace {

// compute_crc32c (void*, size_t) tests
TEST(ChecksumTest, EmptyData) {
    uint32_t crc = compute_crc32c("", 0);
    EXPECT_EQ(crc, 0u);
}

TEST(ChecksumTest, Deterministic) {
    std::string data = "hello";
    uint32_t crc1 = compute_crc32c(data.data(), data.size());
    uint32_t crc2 = compute_crc32c(data.data(), data.size());
    EXPECT_EQ(crc1, crc2);
    EXPECT_NE(crc1, 0u);
}

TEST(ChecksumTest, DifferentDataDifferentChecksum) {
    std::string a = "hello";
    std::string b = "world";
    uint32_t crc_a = compute_crc32c(a.data(), a.size());
    uint32_t crc_b = compute_crc32c(b.data(), b.size());
    EXPECT_NE(crc_a, crc_b);
}

TEST(ChecksumTest, SameContentSameChecksum) {
    std::string a = "test data";
    std::string b = "test data";
    EXPECT_EQ(compute_crc32c(a.data(), a.size()), compute_crc32c(b.data(), b.size()));
}

TEST(ChecksumTest, SingleByteDifference) {
    std::string a = "hello";
    std::string b = "hallo";
    EXPECT_NE(compute_crc32c(a.data(), a.size()), compute_crc32c(b.data(), b.size()));
}

// compute_crc32c (string_view) tests
TEST(ChecksumTest, StringViewOverload) {
    std::string data = "abcdefgh";
    uint32_t crc_ptr = compute_crc32c(data.data(), data.size());
    uint32_t crc_sv = compute_crc32c(std::string_view(data));
    EXPECT_EQ(crc_ptr, crc_sv);
}

TEST(ChecksumTest, StringViewEmpty) {
    uint32_t crc = compute_crc32c(std::string_view{});
    EXPECT_EQ(crc, 0u);
}

// extend_crc32c tests
TEST(ChecksumTest, ExtendEquivalentToFull) {
    std::string part1 = "hello";
    std::string part2 = " world";
    std::string full = "hello world";

    uint32_t crc_full = compute_crc32c(full.data(), full.size());
    uint32_t crc_part1 = compute_crc32c(part1.data(), part1.size());
    uint32_t crc_extended = extend_crc32c(crc_part1, part2.data(), part2.size());

    EXPECT_EQ(crc_full, crc_extended);
}

TEST(ChecksumTest, ExtendWithEmptyData) {
    std::string data = "test";
    uint32_t crc = compute_crc32c(data.data(), data.size());
    uint32_t extended = extend_crc32c(crc, "", 0);
    EXPECT_EQ(crc, extended);
}

TEST(ChecksumTest, ExtendMultipleChunks) {
    std::string chunk1 = "aa";
    std::string chunk2 = "bb";
    std::string chunk3 = "cc";
    std::string full = "aabbcc";

    uint32_t crc = compute_crc32c(chunk1.data(), chunk1.size());
    crc = extend_crc32c(crc, chunk2.data(), chunk2.size());
    crc = extend_crc32c(crc, chunk3.data(), chunk3.size());

    uint32_t expected = compute_crc32c(full.data(), full.size());
    EXPECT_EQ(crc, expected);
}

// verify_crc32c tests
TEST(ChecksumTest, VerifyCorrect) {
    std::string data = "verify me";
    uint32_t crc = compute_crc32c(data.data(), data.size());
    EXPECT_TRUE(verify_crc32c(data.data(), data.size(), crc));
}

TEST(ChecksumTest, VerifyIncorrect) {
    std::string data = "verify me";
    EXPECT_FALSE(verify_crc32c(data.data(), data.size(), 0xDEADBEEF));
}

TEST(ChecksumTest, VerifyAfterCorruption) {
    std::string data = "original";
    uint32_t crc = compute_crc32c(data.data(), data.size());

    data[0] = 'X';
    EXPECT_FALSE(verify_crc32c(data.data(), data.size(), crc));
}

// Large data test
TEST(ChecksumTest, LargeData) {
    std::string large(1024 * 1024, 'A');
    uint32_t crc = compute_crc32c(large.data(), large.size());
    EXPECT_NE(crc, 0u);
    EXPECT_TRUE(verify_crc32c(large.data(), large.size(), crc));
}

// ChecksumType enum
TEST(ChecksumTest, ChecksumTypeValues) {
    EXPECT_EQ(static_cast<uint8_t>(ChecksumType::kNone), 0u);
    EXPECT_EQ(static_cast<uint8_t>(ChecksumType::kCRC32C), 1u);
}

} // namespace
} // namespace pl::minidfs
