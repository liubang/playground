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

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/sstv2/compress/block_compressor.h"
#include "cpp/pl/sstv2/compress/multi_prefix.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::compress;

namespace {

std::span<const std::byte> as_bytes(const std::string& s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

// ===========================================================================
// MultiPrefixCompressorTest
// ===========================================================================

class MultiPrefixCompressorTest : public ::testing::Test {
protected:
    MultiPrefixCompressor compressor_{MultiPrefixConfig{}};
};

TEST_F(MultiPrefixCompressorTest, RoundTrip) {
    std::vector<std::string_view> inputs = {
        "com.example.alpha",
        "com.example.beta",
        "com.example.gamma",
        "com.foo.bar",
        "com.foo.baz",
    };

    auto result = compressor_.compress(std::span<const std::string_view>(inputs));
    ASSERT_TRUE(result.ok()) << result.status().message();

    auto decompressed = MultiPrefixCompressor::decompress_all(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(result->compressed_data.data()),
            result->compressed_data.size()),
        result->prefix_directory,
        inputs.size());
    ASSERT_TRUE(decompressed.ok()) << decompressed.status().message();

    ASSERT_EQ(decompressed->size(), inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        EXPECT_EQ((*decompressed)[i], inputs[i]) << "mismatch at index " << i;
    }
}

TEST_F(MultiPrefixCompressorTest, SingleString) {
    std::vector<std::string_view> inputs = {"only_one_string"};

    auto result = compressor_.compress(std::span<const std::string_view>(inputs));
    ASSERT_TRUE(result.ok()) << result.status().message();

    auto decompressed = MultiPrefixCompressor::decompress_all(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(result->compressed_data.data()),
            result->compressed_data.size()),
        result->prefix_directory,
        inputs.size());
    ASSERT_TRUE(decompressed.ok()) << decompressed.status().message();

    ASSERT_EQ(decompressed->size(), 1);
    EXPECT_EQ((*decompressed)[0], "only_one_string");
}

TEST_F(MultiPrefixCompressorTest, EmptyInput) {
    std::vector<std::string_view> inputs;

    auto result = compressor_.compress(std::span<const std::string_view>(inputs));
    // Either succeeds with empty output or returns a graceful status
    if (result.ok()) {
        EXPECT_TRUE(result->compressed_data.empty());
    }
}

TEST_F(MultiPrefixCompressorTest, DecompressOne) {
    std::vector<std::string_view> inputs = {
        "com.example.alpha",
        "com.example.beta",
        "com.example.gamma",
        "com.foo.bar",
        "com.foo.baz",
    };

    auto result = compressor_.compress(std::span<const std::string_view>(inputs));
    ASSERT_TRUE(result.ok()) << result.status().message();

    for (size_t i = 0; i < inputs.size(); ++i) {
        auto single = MultiPrefixCompressor::decompress_one(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(result->compressed_data.data()),
                result->compressed_data.size()),
            result->prefix_directory,
            inputs.size(),
            i);
        ASSERT_TRUE(single.ok()) << single.status().message();
        EXPECT_EQ(*single, inputs[i]) << "mismatch at index " << i;
    }
}

// ===========================================================================
// BlockCompressorTest
// ===========================================================================

class BlockCompressorTest : public ::testing::Test {
protected:
    std::string MakeLargePayload() {
        std::string payload;
        for (int i = 0; i < 100; ++i) {
            payload += "hello world ";
        }
        return payload;
    }
};

TEST_F(BlockCompressorTest, NoneRoundTrip) {
    std::string data = MakeLargePayload();
    auto compressed = BlockCompressor::compress(CompressionType::kNone, as_bytes(data));
    ASSERT_TRUE(compressed.ok()) << compressed.status().message();

    auto decompressed =
        BlockCompressor::decompress(CompressionType::kNone, as_bytes(*compressed), data.size());
    ASSERT_TRUE(decompressed.ok()) << decompressed.status().message();
    EXPECT_EQ(*decompressed, data);
}

TEST_F(BlockCompressorTest, SnappyRoundTrip) {
    std::string data = MakeLargePayload();
    auto compressed = BlockCompressor::compress(CompressionType::kSnappy, as_bytes(data));
    ASSERT_TRUE(compressed.ok()) << compressed.status().message();

    // Compressed size should be smaller than original for repetitive data
    EXPECT_LT(compressed->size(), data.size());

    auto decompressed =
        BlockCompressor::decompress(CompressionType::kSnappy, as_bytes(*compressed), data.size());
    ASSERT_TRUE(decompressed.ok()) << decompressed.status().message();
    EXPECT_EQ(*decompressed, data);
}

TEST_F(BlockCompressorTest, ZstdRoundTrip) {
    std::string data = MakeLargePayload();
    auto compressed = BlockCompressor::compress(CompressionType::kZstd, as_bytes(data));
    ASSERT_TRUE(compressed.ok()) << compressed.status().message();

    // Compressed size should be smaller than original for repetitive data
    EXPECT_LT(compressed->size(), data.size());

    auto decompressed =
        BlockCompressor::decompress(CompressionType::kZstd, as_bytes(*compressed), data.size());
    ASSERT_TRUE(decompressed.ok()) << decompressed.status().message();
    EXPECT_EQ(*decompressed, data);
}

} // namespace
