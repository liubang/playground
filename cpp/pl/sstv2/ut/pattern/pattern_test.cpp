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

#include "cpp/pl/sstv2/pattern/compound.h"
#include "cpp/pl/sstv2/pattern/raw.h"

#include "cpp/pl/sstv2/codec/endian.h"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>

namespace pl::sstv2::pattern {
namespace {

// =============================================================================
// Pattern 0 (Raw) Tests
// =============================================================================

TEST(RawEncoderTest, Uint8RoundTrip) {
    RawEncoder<1> enc;
    enc.add(uint8_t{0});
    enc.add(uint8_t{42});
    enc.add(uint8_t{255});

    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 3u);

    RawDecoder<1> dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 3u);
    EXPECT_EQ(dec.get(0), 0u);
    EXPECT_EQ(dec.get(1), 42u);
    EXPECT_EQ(dec.get(2), 255u);
    EXPECT_EQ(dec.bytes_consumed(), result.data.size());
}

TEST(RawEncoderTest, Uint16RoundTrip) {
    RawEncoder<2> enc;
    enc.add(uint16_t{0});
    enc.add(uint16_t{1000});
    enc.add(uint16_t{65535});

    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 3u);

    RawDecoder<2> dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 3u);
    EXPECT_EQ(dec.get(0), 0u);
    EXPECT_EQ(dec.get(1), 1000u);
    EXPECT_EQ(dec.get(2), 65535u);
}

TEST(RawEncoderTest, Uint32RoundTrip) {
    RawEncoder<4> enc;
    enc.add(uint32_t{0});
    enc.add(uint32_t{123456789});
    enc.add(uint32_t{0xFFFFFFFF});

    auto result = enc.finish();

    RawDecoder<4> dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 3u);
    EXPECT_EQ(dec.get(0), 0u);
    EXPECT_EQ(dec.get(1), 123456789u);
    EXPECT_EQ(dec.get(2), 0xFFFFFFFFu);
}

TEST(RawEncoderTest, Uint64RoundTrip) {
    RawEncoder<8> enc;
    enc.add(uint64_t{0});
    enc.add(uint64_t{1ULL << 40});
    enc.add(std::numeric_limits<uint64_t>::max());

    auto result = enc.finish();

    RawDecoder<8> dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 3u);
    EXPECT_EQ(dec.get(0), 0u);
    EXPECT_EQ(dec.get(1), 1ULL << 40);
    EXPECT_EQ(dec.get(2), std::numeric_limits<uint64_t>::max());
}

TEST(RawEncoderTest, Int64ViaRawBytes) {
    // Int64 stored as 8-byte little-endian via add(const uint8_t*).
    RawEncoder<8> enc;
    int64_t values[] = {-1, 0, 42, std::numeric_limits<int64_t>::min()};

    for (auto v : values) {
        uint8_t buf[8];
        codec::encode_fixed64(buf, static_cast<int64_t>(v));
        enc.add(buf);
    }

    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 4u);

    RawDecoder<8> dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 4u);

    for (size_t i = 0; i < 4; ++i) {
        int64_t decoded;
        std::memcpy(&decoded, dec.cell(i), 8);
        EXPECT_EQ(decoded, values[i]);
    }
}

TEST(RawEncoderTest, LongDoubleRoundTrip) {
    RawEncoder<16> enc;

    uint8_t data1[16] = {};
    data1[0] = 0x42;
    data1[15] = 0xFF;
    enc.add(data1);

    uint8_t data2[16] = {};
    data2[7] = 0xAB;
    enc.add(data2);

    auto result = enc.finish();

    RawDecoder<16> dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 2u);
    EXPECT_EQ(dec.cell_size(), 16u);
    EXPECT_EQ(std::memcmp(dec.cell(0), data1, 16), 0);
    EXPECT_EQ(std::memcmp(dec.cell(1), data2, 16), 0);
}

TEST(RawEncoderTest, EmptyEncoder) {
    RawEncoder<4> enc;
    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 0u);

    RawDecoder<4> dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 0u);
}

TEST(RawEncoderTest, Reset) {
    RawEncoder<8> enc;
    enc.add(uint64_t{100});
    enc.add(uint64_t{200});
    enc.reset();
    enc.add(uint64_t{300});

    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 1u);

    RawDecoder<8> dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 1u);
    EXPECT_EQ(dec.get(0), 300u);
}

TEST(RawEncoderTest, Reserve) {
    RawEncoder<4> enc;
    enc.reserve(1000);
    for (uint32_t i = 0; i < 1000; ++i) {
        enc.add(i);
    }
    EXPECT_EQ(enc.row_count(), 1000u);
}

TEST(RawEncoderTest, FinishInto) {
    RawEncoder<4> enc;
    enc.add(uint32_t{42});
    enc.add(uint32_t{99});

    std::string buf = "PREFIX";
    enc.finish_into(&buf);

    // buf should contain "PREFIX" + encoded data.
    EXPECT_GT(buf.size(), 6u);

    // Decode from the portion after prefix.
    std::string_view encoded(buf.data() + 6, buf.size() - 6);
    RawDecoder<4> dec;
    ASSERT_TRUE(dec.parse(encoded));
    EXPECT_EQ(dec.row_count(), 2u);
    EXPECT_EQ(dec.get(0), 42u);
    EXPECT_EQ(dec.get(1), 99u);
}

TEST(RawDecoderTest, RejectsWrongPatternId) {
    // Manually craft bytes with wrong pattern_id.
    std::string bad;
    bad.push_back(1); // pattern_id = 1 (StreamVByte), not 0
    bad.push_back(0); // row_count = 0

    RawDecoder<4> dec;
    EXPECT_FALSE(dec.parse(bad));
}

TEST(RawDecoderTest, RejectsTruncatedInput) {
    RawEncoder<8> enc;
    enc.add(uint64_t{42});
    auto result = enc.finish();

    // Truncate the last byte.
    std::string truncated = result.data.substr(0, result.data.size() - 1);

    RawDecoder<8> dec;
    EXPECT_FALSE(dec.parse(truncated));
}

// =============================================================================
// Pattern 100 (Compound) Tests
// =============================================================================

TEST(StringRefEncoderTest, RoundTrip) {
    StringRefEncoder enc;
    enc.add(0, 100);
    enc.add(100, 50);
    enc.add(150, 200);

    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 3u);

    StringRefDecoder dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 3u);

    EXPECT_EQ(dec.offset(0), 0u);
    EXPECT_EQ(dec.length(0), 100u);

    EXPECT_EQ(dec.offset(1), 100u);
    EXPECT_EQ(dec.length(1), 50u);

    EXPECT_EQ(dec.offset(2), 150u);
    EXPECT_EQ(dec.length(2), 200u);
}

TEST(StringRefEncoderTest, EmptyEncoder) {
    StringRefEncoder enc;
    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 0u);

    StringRefDecoder dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 0u);
}

TEST(TimeEncoderTest, RoundTrip) {
    TimeEncoder enc;
    enc.add(1700000000, 123456789);
    enc.add(-1, 0);
    enc.add(0, 999999999);

    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 3u);

    TimeDecoder dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 3u);

    EXPECT_EQ(dec.seconds(0), 1700000000);
    EXPECT_EQ(dec.nanoseconds(0), 123456789u);

    EXPECT_EQ(dec.seconds(1), -1);
    EXPECT_EQ(dec.nanoseconds(1), 0u);

    EXPECT_EQ(dec.seconds(2), 0);
    EXPECT_EQ(dec.nanoseconds(2), 999999999u);
}

TEST(VersionEncoderTest, RoundTrip) {
    VersionEncoder enc;
    enc.add(1, 0);
    enc.add(2, 3);
    enc.add(std::numeric_limits<uint64_t>::max(), 42);

    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 3u);

    VersionDecoder dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 3u);

    EXPECT_EQ(dec.major(0), 1u);
    EXPECT_EQ(dec.minor(0), 0u);

    EXPECT_EQ(dec.major(1), 2u);
    EXPECT_EQ(dec.minor(1), 3u);

    EXPECT_EQ(dec.major(2), std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(dec.minor(2), 42u);
}

TEST(VersionEncoderTest, LargeRowCount) {
    VersionEncoder enc;
    enc.reserve(1000);
    for (uint64_t i = 0; i < 1000; ++i) {
        enc.add(i, i * 2);
    }

    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 1000u);

    VersionDecoder dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 1000u);

    for (uint64_t i = 0; i < 1000; ++i) {
        EXPECT_EQ(dec.major(i), i);
        EXPECT_EQ(dec.minor(i), i * 2);
    }
}

TEST(StringRefDecoderTest, RejectsWrongPatternId) {
    std::string bad;
    bad.push_back(0); // pattern_id = 0 (Raw), not 100
    bad.push_back(2); // sub_column_count = 2

    StringRefDecoder dec;
    EXPECT_FALSE(dec.parse(bad));
}

TEST(StringRefDecoderTest, RejectsMismatchedSubColumnCount) {
    // Craft a compound header claiming 3 sub-columns (expects 2).
    std::string bad;
    bad.push_back(100); // pattern_id = 100
    bad.push_back(3);   // sub_column_count = 3 (wrong)

    StringRefDecoder dec;
    EXPECT_FALSE(dec.parse(bad));
}

TEST(RawEncoderTest, LargeRowCount) {
    RawEncoder<4> enc;
    for (uint32_t i = 0; i < 10000; ++i) {
        enc.add(i);
    }

    auto result = enc.finish();
    EXPECT_EQ(result.row_count, 10000u);

    RawDecoder<4> dec;
    ASSERT_TRUE(dec.parse(result.data));
    EXPECT_EQ(dec.row_count(), 10000u);

    for (uint32_t i = 0; i < 10000; ++i) {
        EXPECT_EQ(dec.get(i), i);
    }
}

// Verify that decoder can handle input with trailing bytes (only consumes what it needs).
TEST(RawDecoderTest, TrailingBytesIgnored) {
    RawEncoder<4> enc;
    enc.add(uint32_t{7});
    auto result = enc.finish();

    // Append garbage.
    std::string with_trailing = result.data + "GARBAGE";

    RawDecoder<4> dec;
    ASSERT_TRUE(dec.parse(with_trailing));
    EXPECT_EQ(dec.row_count(), 1u);
    EXPECT_EQ(dec.get(0), 7u);
    EXPECT_EQ(dec.bytes_consumed(), result.data.size());
}

TEST(VersionDecoderTest, TrailingBytesIgnored) {
    VersionEncoder enc;
    enc.add(10, 20);
    auto result = enc.finish();

    std::string with_trailing = result.data + "EXTRA";

    VersionDecoder dec;
    ASSERT_TRUE(dec.parse(with_trailing));
    EXPECT_EQ(dec.row_count(), 1u);
    EXPECT_EQ(dec.bytes_consumed(), result.data.size());

    EXPECT_EQ(dec.major(0), 10u);
    EXPECT_EQ(dec.minor(0), 20u);
}

TEST(CompoundEncoderTest, FinishInto) {
    StringRefEncoder enc;
    enc.add(0, 10);

    std::string buf = "HDR";
    enc.finish_into(&buf);

    EXPECT_GT(buf.size(), 3u);

    std::string_view encoded(buf.data() + 3, buf.size() - 3);
    StringRefDecoder dec;
    ASSERT_TRUE(dec.parse(encoded));
    EXPECT_EQ(dec.row_count(), 1u);
    EXPECT_EQ(dec.offset(0), 0u);
    EXPECT_EQ(dec.length(0), 10u);
}

} // namespace
} // namespace pl::sstv2::pattern
