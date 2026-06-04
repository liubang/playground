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

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include "cpp/pl/sstv2/encode/stream_vbyte.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::encode;

namespace {

void RoundTrip(const std::vector<uint32_t>& input) {
    if (input.empty()) {
        // Empty case: just verify max_encoded_size is 0
        EXPECT_EQ(StreamVByte::max_encoded_size(0), 0u);
        return;
    }

    size_t max_size = StreamVByte::max_encoded_size(input.size());
    std::vector<std::byte> encoded(max_size);

    size_t written = StreamVByte::encode(input, encoded.data());
    ASSERT_GT(written, 0u);
    ASSERT_LE(written, max_size);

    std::vector<uint32_t> decoded(input.size());
    StreamVByte::decode(
        std::span<const std::byte>(encoded.data(), written), input.size(), decoded.data());

    EXPECT_EQ(decoded, input);
}

} // namespace

TEST(StreamVByteTest, EmptySequence) {
    RoundTrip({});
}

TEST(StreamVByteTest, AllZeros) {
    RoundTrip(std::vector<uint32_t>(8, 0));
}

TEST(StreamVByteTest, AllSmallValues) {
    // All values fit in 1 byte (< 256)
    RoundTrip({1, 2, 3, 100, 200, 255, 0, 127});
}

TEST(StreamVByteTest, AllMaxValues) {
    RoundTrip(std::vector<uint32_t>(8, UINT32_MAX));
}

TEST(StreamVByteTest, MixedSizes) {
    // Mix of 1-byte, 2-byte, 3-byte, and 4-byte values
    RoundTrip({0, 255, 256, 65535, 65536, 16777215, 16777216, UINT32_MAX});
}

TEST(StreamVByteTest, SingleElement) {
    RoundTrip({42});
}

TEST(StreamVByteTest, TwoElements) {
    RoundTrip({0, UINT32_MAX});
}

TEST(StreamVByteTest, ThreeElements) {
    RoundTrip({100, 200, 300});
}

TEST(StreamVByteTest, FourElements) {
    RoundTrip({1, 1000, 1000000, UINT32_MAX});
}

TEST(StreamVByteTest, FiveElements) {
    RoundTrip({0, 1, 2, 3, 4});
}

TEST(StreamVByteTest, EightElements) {
    RoundTrip({10, 1000, 100000, UINT32_MAX, 0, 255, 65535, 16777215});
}

TEST(StreamVByteTest, LargeSequence100) {
    std::vector<uint32_t> data(100);
    std::iota(data.begin(), data.end(), 0);
    RoundTrip(data);
}

TEST(StreamVByteTest, LargeSequence1000) {
    std::vector<uint32_t> data(1000);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint32_t>(i * 1000);
    }
    RoundTrip(data);
}

TEST(StreamVByteTest, EncodedSizeBound) {
    // Verify encoded size is always <= max_encoded_size
    std::vector<uint32_t> data = {0, 255, 65535, UINT32_MAX, 12345};
    size_t max_size = StreamVByte::max_encoded_size(data.size());
    std::vector<std::byte> encoded(max_size);

    size_t written = StreamVByte::encode(data, encoded.data());
    EXPECT_LE(written, max_size);
}
