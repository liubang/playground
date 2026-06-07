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
// Created: 2026/06/05 22:09

#include <gtest/gtest.h>
#include <string>

#include "cpp/pl/sstv2/compress/compress.h"

namespace pl::sstv2::compress {
namespace {

TEST(CompressTest, NoneRoundTrip) {
    const std::string input = "sstable-v2-plain";
    auto encoded = compress(input, Options{.codec = Codec::kNone});
    ASSERT_TRUE(encoded.ok()) << encoded.status();

    auto decoded = uncompress(*encoded, Codec::kNone, input.size());
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(*decoded, input);
}

TEST(CompressTest, SnappyRoundTrip) {
    const std::string input(4096, 'x');
    auto encoded = compress(input, Options{.codec = Codec::kSnappy});
    ASSERT_TRUE(encoded.ok()) << encoded.status();

    auto decoded = uncompress(*encoded, Codec::kSnappy, input.size());
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(*decoded, input);
}

TEST(CompressTest, ZstdRoundTrip) {
    const std::string input = [] {
        std::string s;
        for (int i = 0; i < 1024; ++i)
            s += "row-key-version-optype";
        return s;
    }();

    auto encoded = compress(input, Options{.codec = Codec::kZstd, .zstd_level = 1});
    ASSERT_TRUE(encoded.ok()) << encoded.status();

    auto decoded = uncompress(*encoded, Codec::kZstd, input.size());
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(*decoded, input);
}

TEST(CompressTest, CompileTimeCodecStrategy) {
    const std::string input = "compile-time-snappy-strategy";
    auto encoded = compress_as<Codec::kSnappy>(input);
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    EXPECT_EQ(encoded->codec, Codec::kSnappy);
    EXPECT_EQ(encoded->uncompressed_size, input.size());

    auto decoded =
        CodecImpl<Codec::kSnappy>::uncompress(encoded->view(), encoded->uncompressed_size);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(*decoded, input);
}

TEST(CompressTest, BlockFlagRoundTrip) {
    const uint64_t flags = encode_block_flag(Codec::kZstd);
    EXPECT_NE(flags & block_flags::kPatternStore, 0u);
    EXPECT_EQ(flags & block_flags::kRowKeyBitmap, 0u);
    EXPECT_EQ(flags & block_flags::kCompressMask,
              static_cast<uint64_t>(Codec::kZstd) << block_flags::kCompressShift);
    EXPECT_EQ(decode_block_flag(flags), Codec::kZstd);
}

} // namespace
} // namespace pl::sstv2::compress
