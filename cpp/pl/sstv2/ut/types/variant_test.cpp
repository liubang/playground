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
#include <string>
#include <vector>

#include "cpp/pl/sstv2/types/variant.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::types;

// === Construction and type checking ===

TEST(VariantTest, None) {
    auto v = Variant::none();
    EXPECT_EQ(v.type(), DataType::kNone);
    EXPECT_TRUE(v.is_none());
}

TEST(VariantTest, Boolean) {
    auto t = Variant::boolean(true);
    auto f = Variant::boolean(false);
    EXPECT_EQ(t.type(), DataType::kBool);
    EXPECT_FALSE(t.is_none());
    EXPECT_TRUE(t.as_bool());
    EXPECT_FALSE(f.as_bool());
}

TEST(VariantTest, SignedIntegers) {
    auto v8 = Variant::int8(-1);
    EXPECT_EQ(v8.type(), DataType::kInt8);
    EXPECT_EQ(v8.as_int(), -1);

    auto v16 = Variant::int16(-32000);
    EXPECT_EQ(v16.type(), DataType::kInt16);
    EXPECT_EQ(v16.as_int(), -32000);

    auto v32 = Variant::int32(INT32_MIN);
    EXPECT_EQ(v32.type(), DataType::kInt32);
    EXPECT_EQ(v32.as_int(), INT32_MIN);

    auto v64 = Variant::int64(INT64_MAX);
    EXPECT_EQ(v64.type(), DataType::kInt64);
    EXPECT_EQ(v64.as_int(), INT64_MAX);
}

TEST(VariantTest, UnsignedIntegers) {
    auto v8 = Variant::uint8(255);
    EXPECT_EQ(v8.type(), DataType::kUint8);
    EXPECT_EQ(v8.as_uint(), 255u);

    auto v16 = Variant::uint16(65535);
    EXPECT_EQ(v16.type(), DataType::kUint16);
    EXPECT_EQ(v16.as_uint(), 65535u);

    auto v32 = Variant::uint32(UINT32_MAX);
    EXPECT_EQ(v32.type(), DataType::kUint32);
    EXPECT_EQ(v32.as_uint(), UINT32_MAX);

    auto v64 = Variant::uint64(UINT64_MAX);
    EXPECT_EQ(v64.type(), DataType::kUint64);
    EXPECT_EQ(v64.as_uint(), UINT64_MAX);
}

TEST(VariantTest, FloatingPoint) {
    auto f32 = Variant::float32(3.14f);
    EXPECT_EQ(f32.type(), DataType::kFloat);
    EXPECT_FLOAT_EQ(static_cast<float>(f32.as_float()), 3.14f);

    auto f64 = Variant::float64(2.718281828);
    EXPECT_EQ(f64.type(), DataType::kDouble);
    EXPECT_DOUBLE_EQ(f64.as_float(), 2.718281828);
}

TEST(VariantTest, TimeAndVersion) {
    auto t = Variant::time(1234567890);
    EXPECT_EQ(t.type(), DataType::kTime);
    EXPECT_EQ(t.as_int(), 1234567890);

    auto ver = Variant::version(42);
    EXPECT_EQ(ver.type(), DataType::kVersion);
    EXPECT_EQ(ver.as_uint(), 42u);
}

TEST(VariantTest, String) {
    auto s = Variant::string("hello");
    EXPECT_EQ(s.type(), DataType::kString);
    EXPECT_EQ(s.as_string(), "hello");

    auto empty = Variant::string("");
    EXPECT_EQ(empty.as_string(), "");
}

TEST(VariantTest, Binary) {
    std::vector<std::byte> data = {std::byte{0x01}, std::byte{0x02}, std::byte{0xFF}};
    auto b = Variant::binary(data);
    EXPECT_EQ(b.type(), DataType::kBinary);
    auto span = b.as_binary();
    ASSERT_EQ(span.size(), 3u);
    EXPECT_EQ(span[0], std::byte{0x01});
    EXPECT_EQ(span[2], std::byte{0xFF});
}

// === Comparison ===

TEST(VariantTest, EqualitySameType) {
    EXPECT_EQ(Variant::int32(42), Variant::int32(42));
    EXPECT_NE(Variant::int32(42), Variant::int32(43));
    EXPECT_EQ(Variant::string("abc"), Variant::string("abc"));
    EXPECT_NE(Variant::string("abc"), Variant::string("def"));
    EXPECT_EQ(Variant::boolean(true), Variant::boolean(true));
}

TEST(VariantTest, OrderingSameType) {
    EXPECT_LT(Variant::int32(-1), Variant::int32(0));
    EXPECT_GT(Variant::uint64(100), Variant::uint64(50));
    EXPECT_LT(Variant::string("aaa"), Variant::string("bbb"));
    EXPECT_LT(Variant::float64(1.0), Variant::float64(2.0));
}

TEST(VariantTest, OrderingCrossType) {
    // None should compare as less than non-none
    auto none = Variant::none();
    auto val = Variant::int32(0);
    EXPECT_LT(none, val);
}

// === Copy and Move ===

TEST(VariantTest, CopyConstruct) {
    auto orig = Variant::string("copy_me");
    auto copy = orig; // NOLINT
    EXPECT_EQ(copy.as_string(), "copy_me");
    EXPECT_EQ(orig.as_string(), "copy_me");
}

TEST(VariantTest, MoveConstruct) {
    auto orig = Variant::string("move_me");
    auto moved = std::move(orig);
    EXPECT_EQ(moved.as_string(), "move_me");
}

// === Encode/Decode round-trip ===

TEST(VariantTest, EncodeDecodeFixedTypes) {
    std::vector<Variant> variants = {
        Variant::boolean(true),
        Variant::boolean(false),
        Variant::int8(-128),
        Variant::int16(12345),
        Variant::int32(-999999),
        Variant::int64(INT64_MIN),
        Variant::uint8(0),
        Variant::uint16(65535),
        Variant::uint32(UINT32_MAX),
        Variant::uint64(UINT64_MAX),
        Variant::float32(0.0f),
        Variant::float64(-1.23e10),
        Variant::time(0),
        Variant::version(UINT64_MAX),
    };

    for (const auto& v : variants) {
        std::string buf;
        v.encode_to(buf);
        auto decoded = Variant::decode_from(
            v.type(),
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
        ASSERT_TRUE(decoded.ok()) << decoded.status().message();
        EXPECT_EQ(*decoded, v);
    }
}

TEST(VariantTest, EncodeDecodeString) {
    auto v = Variant::string("hello world");
    std::string buf;
    v.encode_to(buf);
    auto decoded = Variant::decode_from(
        DataType::kString,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded->as_string(), "hello world");
}

TEST(VariantTest, EncodeDecodeEmptyString) {
    auto v = Variant::string("");
    std::string buf;
    v.encode_to(buf);
    auto decoded = Variant::decode_from(
        DataType::kString,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded->as_string(), "");
}

TEST(VariantTest, EncodeDecodeBinary) {
    std::vector<std::byte> data = {
        std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    auto v = Variant::binary(data);
    std::string buf;
    v.encode_to(buf);
    auto decoded = Variant::decode_from(
        DataType::kBinary,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    ASSERT_TRUE(decoded.ok());
    auto span = decoded->as_binary();
    ASSERT_EQ(span.size(), 4u);
    EXPECT_EQ(span[0], std::byte{0xDE});
    EXPECT_EQ(span[3], std::byte{0xEF});
}

// === Edge cases ===

TEST(VariantTest, NegativeNumbers) {
    auto v = Variant::int64(-1);
    EXPECT_EQ(v.as_int(), -1);

    std::string buf;
    v.encode_to(buf);
    auto decoded = Variant::decode_from(
        DataType::kInt64,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded->as_int(), -1);
}
