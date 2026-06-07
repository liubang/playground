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

#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>

#include "cpp/pl/sstv2/codec/comparable.h"
#include "cpp/pl/sstv2/codec/value_comparable.h"

namespace pl::sstv2::codec {
namespace {

// =============================================================================
// Encode order tests.
// =============================================================================

TEST(ComparableUint8Test, OrderPreserving) {
    std::string a, b, c;
    encode_uint8(0, &a);
    encode_uint8(100, &b);
    encode_uint8(255, &c);
    EXPECT_LT(a, b);
    EXPECT_LT(b, c);
}

TEST(ComparableUint16Test, OrderPreserving) {
    std::string a, b, c;
    encode_uint16(0, &a);
    encode_uint16(1000, &b);
    encode_uint16(65535, &c);
    EXPECT_LT(a, b);
    EXPECT_LT(b, c);
}

TEST(ComparableUint32Test, OrderPreserving) {
    std::string a, b;
    encode_uint32(100, &a);
    encode_uint32(200, &b);
    EXPECT_LT(a, b);
}

TEST(ComparableUint64Test, OrderPreserving) {
    std::string a, b, c, d;
    encode_uint64(0, &c);
    encode_uint64(100, &a);
    encode_uint64(200, &b);
    encode_uint64(UINT64_MAX, &d);
    EXPECT_LT(c, a);
    EXPECT_LT(a, b);
    EXPECT_GT(d, b);
}

TEST(ComparableInt8Test, OrderPreserving) {
    std::string neg, zero, pos;
    encode_int8(-128, &neg);
    encode_int8(0, &zero);
    encode_int8(127, &pos);
    EXPECT_LT(neg, zero);
    EXPECT_LT(zero, pos);
}

TEST(ComparableInt16Test, OrderPreserving) {
    std::string neg, zero, pos;
    encode_int16(-1000, &neg);
    encode_int16(0, &zero);
    encode_int16(1000, &pos);
    EXPECT_LT(neg, zero);
    EXPECT_LT(zero, pos);
}

TEST(ComparableInt32Test, OrderPreserving) {
    std::string neg, zero, pos;
    encode_int32(-1, &neg);
    encode_int32(0, &zero);
    encode_int32(1, &pos);
    EXPECT_LT(neg, zero);
    EXPECT_LT(zero, pos);
}

TEST(ComparableInt64Test, OrderPreserving) {
    std::string neg, zero, pos, min_val, max_val;
    encode_int64(-100, &neg);
    encode_int64(0, &zero);
    encode_int64(100, &pos);
    encode_int64(INT64_MIN, &min_val);
    encode_int64(INT64_MAX, &max_val);
    EXPECT_LT(neg, zero);
    EXPECT_LT(zero, pos);
    EXPECT_LT(min_val, neg);
    EXPECT_GT(max_val, pos);
}

TEST(ComparableFloatTest, OrderPreserving) {
    std::string neg_inf, neg, neg_zero, zero, pos, pos_inf;
    encode_float(-std::numeric_limits<float>::infinity(), &neg_inf);
    encode_float(-1.5f, &neg);
    encode_float(-0.0f, &neg_zero);
    encode_float(0.0f, &zero);
    encode_float(1.5f, &pos);
    encode_float(std::numeric_limits<float>::infinity(), &pos_inf);
    EXPECT_LT(neg_inf, neg);
    EXPECT_LT(neg, neg_zero);
    EXPECT_LE(neg_zero, zero); // -0 <= +0
    EXPECT_LT(zero, pos);
    EXPECT_LT(pos, pos_inf);
}

TEST(ComparableDoubleTest, OrderPreserving) {
    std::string neg_inf, neg, zero, pos, pos_inf;
    encode_double(-std::numeric_limits<double>::infinity(), &neg_inf);
    encode_double(-2.718, &neg);
    encode_double(0.0, &zero);
    encode_double(3.14, &pos);
    encode_double(std::numeric_limits<double>::infinity(), &pos_inf);
    EXPECT_LT(neg_inf, neg);
    EXPECT_LT(neg, zero);
    EXPECT_LT(zero, pos);
    EXPECT_LT(pos, pos_inf);
}

TEST(ComparableBytesTest, OrderPreserving) {
    std::string a, b, c;
    encode_bytes("abc", &a);
    encode_bytes("abd", &b);
    encode_bytes("abcd", &c);
    EXPECT_LT(a, b);
    EXPECT_LT(a, c);
}

TEST(ComparableBytesTest, EmptyString) {
    std::string empty, nonempty;
    encode_bytes("", &empty);
    encode_bytes("a", &nonempty);
    EXPECT_LT(empty, nonempty);
}

TEST(ComparableBytesTest, LongString) {
    std::string a, b;
    encode_bytes("12345678X", &a);
    encode_bytes("12345678Y", &b);
    EXPECT_LT(a, b);
}

TEST(ComparableMapTest, CanonicalInsertionOrder) {
    using types::DataType;
    using types::SortOrder;
    using types::Value;

    Value left = Value::make_map({
        {Value::make<DataType::kString>("b"), Value::make<DataType::kInt64>(int64_t{2})},
        {Value::make<DataType::kString>("a"), Value::make<DataType::kInt64>(int64_t{1})},
    });
    Value right = Value::make_map({
        {Value::make<DataType::kString>("a"), Value::make<DataType::kInt64>(int64_t{1})},
        {Value::make<DataType::kString>("b"), Value::make<DataType::kInt64>(int64_t{2})},
    });

    std::string left_key;
    std::string right_key;
    ASSERT_TRUE(
        encode_value_comparable(left, DataType::kMap, SortOrder::kAscending, &left_key).ok());
    ASSERT_TRUE(
        encode_value_comparable(right, DataType::kMap, SortOrder::kAscending, &right_key).ok());
    EXPECT_EQ(left_key, right_key);
}

// =============================================================================
// Descending order tests.
// =============================================================================

TEST(ComparableDescTest, Uint8Reversed) {
    std::string a, b;
    encode_uint8_desc(50, &a);
    encode_uint8_desc(100, &b);
    EXPECT_GT(a, b);
}

TEST(ComparableDescTest, Uint64Reversed) {
    std::string a, b;
    encode_uint64_desc(100, &a);
    encode_uint64_desc(200, &b);
    EXPECT_GT(a, b);
}

TEST(ComparableDescTest, Int64Reversed) {
    std::string neg, zero, pos;
    encode_int64_desc(-100, &neg);
    encode_int64_desc(0, &zero);
    encode_int64_desc(100, &pos);
    EXPECT_GT(neg, zero);
    EXPECT_GT(zero, pos);
}

TEST(ComparableDescTest, FloatReversed) {
    std::string a, b;
    encode_float_desc(1.0f, &a);
    encode_float_desc(2.0f, &b);
    EXPECT_GT(a, b);
}

TEST(ComparableDescTest, DoubleReversed) {
    std::string a, b;
    encode_double_desc(1.0, &a);
    encode_double_desc(2.0, &b);
    EXPECT_GT(a, b);
}

TEST(ComparableDescTest, BytesReversed) {
    std::string a, b;
    encode_bytes_desc("abc", &a);
    encode_bytes_desc("abd", &b);
    EXPECT_GT(a, b);
}

// =============================================================================
// Decode roundtrip tests.
// =============================================================================

TEST(ComparableDecodeTest, Uint8Roundtrip) {
    for (uint8_t v : {uint8_t{0}, uint8_t{1}, uint8_t{127}, uint8_t{255}}) {
        std::string enc;
        encode_uint8(v, &enc);
        uint8_t decoded = 0;
        size_t n = decode_uint8(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 1u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, Uint16Roundtrip) {
    for (uint16_t v : {uint16_t{0}, uint16_t{256}, uint16_t{65535}}) {
        std::string enc;
        encode_uint16(v, &enc);
        uint16_t decoded = 0;
        size_t n =
            decode_uint16(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 2u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, Uint32Roundtrip) {
    for (uint32_t v : {0u, 1u, 1000000u, UINT32_MAX}) {
        std::string enc;
        encode_uint32(v, &enc);
        uint32_t decoded = 0;
        size_t n =
            decode_uint32(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, Uint64Roundtrip) {
    for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{1000000000000ULL}, UINT64_MAX}) {
        std::string enc;
        encode_uint64(v, &enc);
        uint64_t decoded = 0;
        size_t n =
            decode_uint64(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 8u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, Int8Roundtrip) {
    for (int8_t v : {int8_t{-128}, int8_t{-1}, int8_t{0}, int8_t{1}, int8_t{127}}) {
        std::string enc;
        encode_int8(v, &enc);
        int8_t decoded = 0;
        size_t n = decode_int8(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 1u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, Int16Roundtrip) {
    for (int16_t v : {int16_t{-32768}, int16_t{-1}, int16_t{0}, int16_t{32767}}) {
        std::string enc;
        encode_int16(v, &enc);
        int16_t decoded = 0;
        size_t n = decode_int16(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 2u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, Int32Roundtrip) {
    for (int32_t v : {INT32_MIN, -1, 0, 1, INT32_MAX}) {
        std::string enc;
        encode_int32(v, &enc);
        int32_t decoded = 0;
        size_t n = decode_int32(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, Int64Roundtrip) {
    for (int64_t v : {INT64_MIN, int64_t{-1}, int64_t{0}, int64_t{1}, INT64_MAX}) {
        std::string enc;
        encode_int64(v, &enc);
        int64_t decoded = 0;
        size_t n = decode_int64(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 8u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, FloatRoundtrip) {
    for (float v : {-std::numeric_limits<float>::infinity(),
                    -1.5f,
                    -0.0f,
                    0.0f,
                    1.5f,
                    std::numeric_limits<float>::infinity()}) {
        std::string enc;
        encode_float(v, &enc);
        float decoded = 0;
        size_t n = decode_float(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 4u);
        // -0 and +0 compare equal as floats.
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, DoubleRoundtrip) {
    for (double v : {-std::numeric_limits<double>::infinity(),
                     -2.718,
                     0.0,
                     3.14,
                     std::numeric_limits<double>::infinity()}) {
        std::string enc;
        encode_double(v, &enc);
        double decoded = 0;
        size_t n =
            decode_double(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 8u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDecodeTest, BytesRoundtrip) {
    for (std::string_view sv : {"", "a", "hello", "12345678", "123456789", "abcdefghijklmnop"}) {
        std::string enc;
        encode_bytes(sv, &enc);
        std::string decoded;
        size_t n = decode_bytes(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_GT(n, 0u);
        EXPECT_EQ(decoded, sv);
    }
}

TEST(ComparableDecodeTest, BytesBinaryData) {
    // Test with embedded nulls.
    std::string data = std::string("\x00\x01\x02\x03", 4);
    std::string enc;
    encode_bytes(data, &enc);
    std::string decoded;
    size_t n = decode_bytes(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
    EXPECT_GT(n, 0u);
    EXPECT_EQ(decoded, data);
}

// =============================================================================
// Descending decode roundtrip tests.
// =============================================================================

TEST(ComparableDescDecodeTest, Uint64Roundtrip) {
    for (uint64_t v : {uint64_t{0}, uint64_t{12345}, UINT64_MAX}) {
        std::string enc;
        encode_uint64_desc(v, &enc);
        uint64_t decoded = 0;
        size_t n =
            decode_uint64_desc(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 8u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDescDecodeTest, Int64Roundtrip) {
    for (int64_t v : {INT64_MIN, int64_t{-1}, int64_t{0}, int64_t{1}, INT64_MAX}) {
        std::string enc;
        encode_int64_desc(v, &enc);
        int64_t decoded = 0;
        size_t n =
            decode_int64_desc(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 8u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDescDecodeTest, FloatRoundtrip) {
    for (float v : {-1.0f, 0.0f, 1.0f}) {
        std::string enc;
        encode_float_desc(v, &enc);
        float decoded = 0;
        size_t n =
            decode_float_desc(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDescDecodeTest, DoubleRoundtrip) {
    for (double v : {-3.14, 0.0, 2.718}) {
        std::string enc;
        encode_double_desc(v, &enc);
        double decoded = 0;
        size_t n =
            decode_double_desc(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_EQ(n, 8u);
        EXPECT_EQ(decoded, v);
    }
}

TEST(ComparableDescDecodeTest, BytesRoundtrip) {
    for (std::string_view sv : {"", "hello", "12345678X"}) {
        std::string enc;
        encode_bytes_desc(sv, &enc);
        std::string decoded;
        size_t n =
            decode_bytes_desc(reinterpret_cast<const uint8_t*>(enc.data()), enc.size(), &decoded);
        EXPECT_GT(n, 0u);
        EXPECT_EQ(decoded, sv);
    }
}

// =============================================================================
// Error handling tests.
// =============================================================================

TEST(ComparableDecodeTest, InsufficientData) {
    uint64_t v64 = 0;
    EXPECT_EQ(decode_uint64(nullptr, 0, &v64), 0u);
    EXPECT_EQ(decode_uint64(nullptr, 7, &v64), 0u);

    uint32_t v32 = 0;
    EXPECT_EQ(decode_uint32(nullptr, 3, &v32), 0u);

    float vf = 0;
    EXPECT_EQ(decode_float(nullptr, 3, &vf), 0u);
}

} // namespace
} // namespace pl::sstv2::codec
