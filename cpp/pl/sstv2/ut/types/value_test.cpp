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

#include "cpp/pl/sstv2/types/value.h"

#include <gtest/gtest.h>

#include <string>

namespace pl::sstv2::types {
namespace {

TEST(ValueTest, DefaultIsNull) {
    Value v;
    EXPECT_TRUE(v.is_null());
    EXPECT_EQ(v.type(), DataType::kNone);
}

TEST(ValueTest, Bool) {
    auto t = Value::make<DataType::kBool>(true);
    auto f = Value::make<DataType::kBool>(false);
    EXPECT_EQ(t.type(), DataType::kBool);
    EXPECT_TRUE(t.get<DataType::kBool>());
    EXPECT_FALSE(f.get<DataType::kBool>());
    // Convenience accessor
    EXPECT_TRUE(t.as_bool());
    EXPECT_FALSE(f.as_bool());
}

TEST(ValueTest, Integers) {
    auto i8 = Value::make<DataType::kInt8>(int8_t{-42});
    EXPECT_EQ(i8.get<DataType::kInt8>(), -42);

    auto u8 = Value::make<DataType::kUint8>(uint8_t{200});
    EXPECT_EQ(u8.get<DataType::kUint8>(), 200);

    auto i16 = Value::make<DataType::kInt16>(int16_t{-1000});
    EXPECT_EQ(i16.get<DataType::kInt16>(), -1000);

    auto u16 = Value::make<DataType::kUint16>(uint16_t{60000});
    EXPECT_EQ(u16.get<DataType::kUint16>(), 60000);

    auto i32 = Value::make<DataType::kInt32>(int32_t{-100000});
    EXPECT_EQ(i32.get<DataType::kInt32>(), -100000);

    auto u32 = Value::make<DataType::kUint32>(uint32_t{3000000000u});
    EXPECT_EQ(u32.get<DataType::kUint32>(), 3000000000u);

    auto i64 = Value::make<DataType::kInt64>(int64_t{-9876543210LL});
    EXPECT_EQ(i64.get<DataType::kInt64>(), -9876543210LL);

    auto u64 = Value::make<DataType::kUint64>(UINT64_MAX);
    EXPECT_EQ(u64.get<DataType::kUint64>(), UINT64_MAX);
}

TEST(ValueTest, FloatingPoint) {
    auto f = Value::make<DataType::kFloat>(3.14f);
    EXPECT_EQ(f.type(), DataType::kFloat);
    EXPECT_FLOAT_EQ(f.get<DataType::kFloat>(), 3.14f);

    auto d = Value::make<DataType::kDouble>(2.718281828);
    EXPECT_EQ(d.type(), DataType::kDouble);
    EXPECT_DOUBLE_EQ(d.get<DataType::kDouble>(), 2.718281828);
}

TEST(ValueTest, String) {
    auto s = Value::make<DataType::kString>("hello world");
    EXPECT_EQ(s.type(), DataType::kString);
    EXPECT_EQ(s.ref<DataType::kString>(), "hello world");
    EXPECT_EQ(s.as_string(), "hello world");
}

TEST(ValueTest, Binary) {
    std::string data = "\x00\x01\x02\x03";
    data.push_back('\0');
    auto b = Value::make<DataType::kBinary>(data);
    EXPECT_EQ(b.type(), DataType::kBinary);
    EXPECT_EQ(b.as_binary(), data);
}

TEST(ValueTest, CopySemantics) {
    auto original = Value::make<DataType::kString>("test");
    Value copy = original; // NOLINT
    EXPECT_EQ(copy.type(), DataType::kString);
    EXPECT_EQ(copy.as_string(), "test");
    EXPECT_EQ(original.as_string(), "test");
}

TEST(ValueTest, MoveSemantics) {
    auto original = Value::make<DataType::kString>("moveme");
    Value moved = std::move(original);
    EXPECT_EQ(moved.type(), DataType::kString);
    EXPECT_EQ(moved.as_string(), "moveme");
}

TEST(ValueTest, TakeString) {
    auto v = Value::make<DataType::kString>("take this");
    std::string taken = v.take<DataType::kString>();
    EXPECT_EQ(taken, "take this");
    EXPECT_TRUE(v.is_null());
}

TEST(ValueTest, MakeValueHelper) {
    // Scalar type deduction via make_value<T>
    auto v1 = make_value(int64_t{99});
    EXPECT_EQ(v1.type(), DataType::kInt64);
    EXPECT_EQ(v1.get<DataType::kInt64>(), 99);

    auto v2 = make_value(true);
    EXPECT_EQ(v2.type(), DataType::kBool);
    EXPECT_TRUE(v2.get<DataType::kBool>());

    auto v3 = make_value(3.14);
    EXPECT_EQ(v3.type(), DataType::kDouble);
    EXPECT_DOUBLE_EQ(v3.get<DataType::kDouble>(), 3.14);
}

TEST(ValueTest, Visitor) {
    auto v = Value::make<DataType::kInt32>(int32_t{42});
    bool visited = false;
    v.visit([&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, int32_t>) {
            EXPECT_EQ(val, 42);
            visited = true;
        }
    });
    EXPECT_TRUE(visited);
}

TEST(ValueTest, ConvenienceAccessorsMatchTemplated) {
    auto v = Value::make<DataType::kUint64>(uint64_t{12345});
    EXPECT_EQ(v.as_uint64(), v.get<DataType::kUint64>());

    auto s = Value::make<DataType::kString>("abc");
    EXPECT_EQ(s.as_string(), s.ref<DataType::kString>());
}

} // namespace
} // namespace pl::sstv2::types
