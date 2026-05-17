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
// Created: 2026/05/17 22:41

#include "cpp/pl/random/random.h"
#include "cpp/pl/sst/compression.h"
#include <gtest/gtest.h>
#include <string>

namespace pl {

class CompressionTest : public ::testing::Test {};

// ==================== Snappy Tests ====================

TEST_F(CompressionTest, snappy_roundtrip_basic) {
    SnappyCompressionAdapter adapter;
    std::string input = "hello world, this is a test string for snappy compression";
    std::string compressed;
    std::string decompressed;

    auto r1 = adapter.compress(input, &compressed);
    ASSERT_TRUE(r1.hasValue());
    EXPECT_FALSE(compressed.empty());
    EXPECT_NE(compressed, input);

    auto r2 = adapter.uncompress(compressed, &decompressed);
    ASSERT_TRUE(r2.hasValue());
    EXPECT_EQ(decompressed, input);
}

TEST_F(CompressionTest, snappy_roundtrip_empty) {
    SnappyCompressionAdapter adapter;
    std::string input;
    std::string compressed;
    std::string decompressed;

    auto r1 = adapter.compress(input, &compressed);
    ASSERT_TRUE(r1.hasValue());

    auto r2 = adapter.uncompress(compressed, &decompressed);
    ASSERT_TRUE(r2.hasValue());
    EXPECT_EQ(decompressed, input);
}

TEST_F(CompressionTest, snappy_roundtrip_large_random_data) {
    SnappyCompressionAdapter adapter;
    std::string input = random_string(128 * 1024); // 128KB
    std::string compressed;
    std::string decompressed;

    auto r1 = adapter.compress(input, &compressed);
    ASSERT_TRUE(r1.hasValue());

    auto r2 = adapter.uncompress(compressed, &decompressed);
    ASSERT_TRUE(r2.hasValue());
    EXPECT_EQ(decompressed, input);
}

TEST_F(CompressionTest, snappy_uncompress_invalid_data) {
    SnappyCompressionAdapter adapter;
    std::string garbage = "this is not valid snappy data!@#$%^";
    std::string output;

    auto r = adapter.uncompress(garbage, &output);
    EXPECT_TRUE(r.hasError());
    EXPECT_EQ(r.error().code(), StatusCode::kDataCorruption);
}

TEST_F(CompressionTest, snappy_uncompress_truncated_data) {
    SnappyCompressionAdapter adapter;
    std::string input = "a valid input to compress first";
    std::string compressed;
    std::string output;

    auto r1 = adapter.compress(input, &compressed);
    ASSERT_TRUE(r1.hasValue());

    // Truncate the compressed data
    std::string truncated = compressed.substr(0, compressed.size() / 2);
    auto r2 = adapter.uncompress(truncated, &output);
    EXPECT_TRUE(r2.hasError());
    EXPECT_EQ(r2.error().code(), StatusCode::kDataCorruption);
}

// ==================== Zstd Tests ====================

TEST_F(CompressionTest, zstd_roundtrip_basic) {
    ZstdCompressionAdapter adapter;
    std::string input = "hello world, this is a test string for zstd compression";
    std::string compressed;
    std::string decompressed;

    auto r1 = adapter.compress(input, &compressed);
    ASSERT_TRUE(r1.hasValue());
    EXPECT_FALSE(compressed.empty());
    EXPECT_NE(compressed, input);

    auto r2 = adapter.uncompress(compressed, &decompressed);
    ASSERT_TRUE(r2.hasValue());
    EXPECT_EQ(decompressed, input);
}

TEST_F(CompressionTest, zstd_roundtrip_empty) {
    ZstdCompressionAdapter adapter;
    std::string input;
    std::string compressed;
    std::string decompressed;

    auto r1 = adapter.compress(input, &compressed);
    ASSERT_TRUE(r1.hasValue());

    auto r2 = adapter.uncompress(compressed, &decompressed);
    ASSERT_TRUE(r2.hasValue());
    EXPECT_EQ(decompressed, input);
}

TEST_F(CompressionTest, zstd_roundtrip_large_random_data) {
    ZstdCompressionAdapter adapter;
    std::string input = random_string(128 * 1024); // 128KB
    std::string compressed;
    std::string decompressed;

    auto r1 = adapter.compress(input, &compressed);
    ASSERT_TRUE(r1.hasValue());

    auto r2 = adapter.uncompress(compressed, &decompressed);
    ASSERT_TRUE(r2.hasValue());
    EXPECT_EQ(decompressed, input);
}

TEST_F(CompressionTest, zstd_uncompress_invalid_data) {
    ZstdCompressionAdapter adapter;
    std::string garbage = "this is not valid zstd data!@#$%^";
    std::string output;

    auto r = adapter.uncompress(garbage, &output);
    EXPECT_TRUE(r.hasError());
    EXPECT_EQ(r.error().code(), StatusCode::kDataCorruption);
}

TEST_F(CompressionTest, zstd_uncompress_truncated_data) {
    ZstdCompressionAdapter adapter;
    std::string input = "a valid input to compress first for zstd";
    std::string compressed;
    std::string output;

    auto r1 = adapter.compress(input, &compressed);
    ASSERT_TRUE(r1.hasValue());

    // Truncate the compressed data
    std::string truncated = compressed.substr(0, compressed.size() / 2);
    auto r2 = adapter.uncompress(truncated, &output);
    EXPECT_TRUE(r2.hasError());
    EXPECT_EQ(r2.error().code(), StatusCode::kDataCorruption);
}

TEST_F(CompressionTest, zstd_compress_ratio) {
    ZstdCompressionAdapter adapter;
    // Highly compressible data (repeated pattern)
    std::string input(4096, 'A');
    std::string compressed;

    auto r = adapter.compress(input, &compressed);
    ASSERT_TRUE(r.hasValue());
    // Compression should reduce size significantly for repetitive data
    EXPECT_LT(compressed.size(), input.size());
}

// ==================== ISAL Tests ====================

TEST_F(CompressionTest, isal_compress_not_implemented) {
    IsalCompressionAdapter adapter;
    std::string input = "test data";
    std::string output;

    auto r = adapter.compress(input, &output);
    EXPECT_TRUE(r.hasError());
    EXPECT_EQ(r.error().code(), StatusCode::kNotImplemented);
}

TEST_F(CompressionTest, isal_uncompress_not_implemented) {
    IsalCompressionAdapter adapter;
    std::string input = "test data";
    std::string output;

    auto r = adapter.uncompress(input, &output);
    EXPECT_TRUE(r.hasError());
    EXPECT_EQ(r.error().code(), StatusCode::kNotImplemented);
}

// ==================== Multiple Rounds ====================

TEST_F(CompressionTest, snappy_multiple_compress_same_adapter) {
    SnappyCompressionAdapter adapter;
    for (int i = 0; i < 5; ++i) {
        std::string input = random_string(1024 + i * 512);
        std::string compressed;
        std::string decompressed;

        auto r1 = adapter.compress(input, &compressed);
        ASSERT_TRUE(r1.hasValue());

        auto r2 = adapter.uncompress(compressed, &decompressed);
        ASSERT_TRUE(r2.hasValue());
        EXPECT_EQ(decompressed, input);
    }
}

TEST_F(CompressionTest, zstd_multiple_compress_same_adapter) {
    ZstdCompressionAdapter adapter;
    for (int i = 0; i < 5; ++i) {
        std::string input = random_string(1024 + i * 512);
        std::string compressed;
        std::string decompressed;

        auto r1 = adapter.compress(input, &compressed);
        ASSERT_TRUE(r1.hasValue());

        auto r2 = adapter.uncompress(compressed, &decompressed);
        ASSERT_TRUE(r2.hasValue());
        EXPECT_EQ(decompressed, input);
    }
}

} // namespace pl
