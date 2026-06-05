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

#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {
namespace {

TEST(ValueTest, DefaultIsNull) {
    Value v;
    EXPECT_TRUE(v.is_null());
    EXPECT_EQ(v.type(), DataType::kNone);
    EXPECT_EQ(v.category(), StorageCategory::kNone);
}

TEST(ValueTest, Bool) {
    auto t = Value::make<DataType::kBool>(true);
    auto f = Value::make<DataType::kBool>(false);
    EXPECT_EQ(t.type(), DataType::kBool);
    EXPECT_TRUE(t.get<DataType::kBool>());
    EXPECT_FALSE(f.get<DataType::kBool>());
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

TEST(ValueTest, LongDouble) {
    LongDouble ld{};
    ld.data[0] = 0xAB;
    ld.data[15] = 0xCD;
    auto v = Value::make<DataType::kLongDouble>(ld);
    EXPECT_EQ(v.type(), DataType::kLongDouble);
    EXPECT_EQ(v.category(), StorageCategory::kInline);
    EXPECT_EQ(v.as_long_double().data[0], 0xAB);
    EXPECT_EQ(v.as_long_double().data[15], 0xCD);
}

TEST(ValueTest, Time) {
    Time t{.seconds = 1700000000, .nanoseconds = 123456789};
    auto v = Value::make<DataType::kTime>(t);
    EXPECT_EQ(v.type(), DataType::kTime);
    EXPECT_EQ(v.as_time().seconds, 1700000000);
    EXPECT_EQ(v.as_time().nanoseconds, 123456789u);
}

TEST(ValueTest, Version) {
    Version ver{.major = 3, .minor = 14};
    auto v = Value::make<DataType::kVersion>(ver);
    EXPECT_EQ(v.type(), DataType::kVersion);
    EXPECT_EQ(v.as_version().major, 3u);
    EXPECT_EQ(v.as_version().minor, 14u);
}

TEST(ValueTest, String) {
    auto s = Value::make<DataType::kString>("hello world");
    EXPECT_EQ(s.type(), DataType::kString);
    EXPECT_EQ(s.ref<DataType::kString>(), "hello world");
    EXPECT_EQ(s.as_string(), "hello world");
}

TEST(ValueTest, U16String) {
    auto v = Value::make<DataType::kU16String>("utf16 data");
    EXPECT_EQ(v.type(), DataType::kU16String);
    EXPECT_EQ(v.category(), StorageCategory::kString);
    EXPECT_EQ(v.as_string(), "utf16 data");
}

TEST(ValueTest, U32String) {
    auto v = Value::make<DataType::kU32String>("utf32 data");
    EXPECT_EQ(v.type(), DataType::kU32String);
    EXPECT_EQ(v.category(), StorageCategory::kString);
    EXPECT_EQ(v.as_string(), "utf32 data");
}

TEST(ValueTest, Binary) {
    std::string data = "\x00\x01\x02\x03";
    data.push_back('\0');
    auto b = Value::make<DataType::kBinary>(data);
    EXPECT_EQ(b.type(), DataType::kBinary);
    EXPECT_EQ(b.as_binary(), data);
}

TEST(ValueTest, Array) {
    std::vector<Value> elems;
    elems.push_back(Value::make<DataType::kInt32>(int32_t{10}));
    elems.push_back(Value::make<DataType::kInt32>(int32_t{20}));
    elems.push_back(Value::make<DataType::kInt32>(int32_t{30}));

    auto v = Value::make_array(std::move(elems));
    EXPECT_EQ(v.type(), DataType::kArray);
    EXPECT_EQ(v.category(), StorageCategory::kArray);
    EXPECT_EQ(v.as_array().size(), 3u);
    EXPECT_EQ(v.as_array()[0].as_int32(), 10);
    EXPECT_EQ(v.as_array()[1].as_int32(), 20);
    EXPECT_EQ(v.as_array()[2].as_int32(), 30);
}

TEST(ValueTest, Map) {
    std::vector<std::pair<Value, Value>> entries;
    entries.emplace_back(Value::make<DataType::kString>("key1"),
                         Value::make<DataType::kInt64>(int64_t{100}));
    entries.emplace_back(Value::make<DataType::kString>("key2"),
                         Value::make<DataType::kInt64>(int64_t{200}));

    auto v = Value::make_map(std::move(entries));
    EXPECT_EQ(v.type(), DataType::kMap);
    EXPECT_EQ(v.category(), StorageCategory::kMap);
    EXPECT_EQ(v.as_map().size(), 2u);
    EXPECT_EQ(v.as_map()[0].first.as_string(), "key1");
    EXPECT_EQ(v.as_map()[0].second.as_int64(), 100);
    EXPECT_EQ(v.as_map()[1].first.as_string(), "key2");
    EXPECT_EQ(v.as_map()[1].second.as_int64(), 200);
}

TEST(ValueTest, TakeArray) {
    std::vector<Value> elems;
    elems.push_back(Value::make<DataType::kString>("a"));
    elems.push_back(Value::make<DataType::kString>("b"));
    auto v = Value::make_array(std::move(elems));

    auto taken = v.take_array();
    EXPECT_TRUE(v.is_null());
    EXPECT_EQ(taken.size(), 2u);
    EXPECT_EQ(taken[0].as_string(), "a");
}

TEST(ValueTest, TakeMap) {
    std::vector<std::pair<Value, Value>> entries;
    entries.emplace_back(Value::make<DataType::kInt32>(int32_t{1}),
                         Value::make<DataType::kString>("one"));
    auto v = Value::make_map(std::move(entries));

    auto taken = v.take_map();
    EXPECT_TRUE(v.is_null());
    EXPECT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].second.as_string(), "one");
}

TEST(ValueTest, CopySemantics) {
    auto original = Value::make<DataType::kString>("test");
    Value copy = original; // NOLINT
    EXPECT_EQ(copy.type(), DataType::kString);
    EXPECT_EQ(copy.as_string(), "test");
    EXPECT_EQ(original.as_string(), "test");
}

TEST(ValueTest, CopyArray) {
    std::vector<Value> elems;
    elems.push_back(Value::make<DataType::kInt32>(int32_t{7}));
    auto original = Value::make_array(std::move(elems));

    Value copy = original; // NOLINT
    EXPECT_EQ(copy.as_array().size(), 1u);
    EXPECT_EQ(copy.as_array()[0].as_int32(), 7);
    EXPECT_EQ(original.as_array()[0].as_int32(), 7);
}

TEST(ValueTest, MoveSemantics) {
    auto original = Value::make<DataType::kString>("moveme");
    Value moved = std::move(original);
    EXPECT_EQ(moved.type(), DataType::kString);
    EXPECT_EQ(moved.as_string(), "moveme");
}

TEST(ValueTest, MoveArray) {
    std::vector<Value> elems;
    elems.push_back(Value::make<DataType::kString>("x"));
    auto original = Value::make_array(std::move(elems));

    Value moved = std::move(original);
    EXPECT_EQ(moved.type(), DataType::kArray);
    EXPECT_EQ(moved.as_array().size(), 1u);
    EXPECT_EQ(moved.as_array()[0].as_string(), "x");
    EXPECT_TRUE(original.is_null()); // NOLINT
}

TEST(ValueTest, TakeString) {
    auto v = Value::make<DataType::kString>("take this");
    std::string taken = v.take<DataType::kString>();
    EXPECT_EQ(taken, "take this");
    EXPECT_TRUE(v.is_null());
}

TEST(ValueTest, MakeValueHelper) {
    auto v1 = make_value(int64_t{99});
    EXPECT_EQ(v1.type(), DataType::kInt64);
    EXPECT_EQ(v1.get<DataType::kInt64>(), 99);

    auto v2 = make_value(true);
    EXPECT_EQ(v2.type(), DataType::kBool);
    EXPECT_TRUE(v2.get<DataType::kBool>());

    auto v3 = make_value(3.14);
    EXPECT_EQ(v3.type(), DataType::kDouble);
    EXPECT_DOUBLE_EQ(v3.get<DataType::kDouble>(), 3.14);

    auto v4 = make_value(Time{.seconds = 42, .nanoseconds = 0});
    EXPECT_EQ(v4.type(), DataType::kTime);
    EXPECT_EQ(v4.as_time().seconds, 42);

    auto v5 = make_value(Version{.major = 1, .minor = 2});
    EXPECT_EQ(v5.type(), DataType::kVersion);
    EXPECT_EQ(v5.as_version().major, 1u);
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

TEST(ValueTest, VisitorArray) {
    std::vector<Value> elems;
    elems.push_back(Value::make<DataType::kInt32>(int32_t{5}));
    auto v = Value::make_array(std::move(elems));

    bool visited = false;
    v.visit([&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, ArrayStorage>) {
            EXPECT_EQ(val.size(), 1u);
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

TEST(ValueTest, AssignmentOverwritesDifferentCategory) {
    // Assign string over inline.
    Value v = Value::make<DataType::kInt32>(int32_t{1});
    v = Value::make<DataType::kString>("overwrite");
    EXPECT_EQ(v.type(), DataType::kString);
    EXPECT_EQ(v.as_string(), "overwrite");

    // Assign array over string.
    std::vector<Value> elems;
    elems.push_back(Value::make<DataType::kBool>(true));
    v = Value::make_array(std::move(elems));
    EXPECT_EQ(v.type(), DataType::kArray);
    EXPECT_EQ(v.as_array().size(), 1u);

    // Assign inline over array.
    v = Value::make<DataType::kDouble>(9.9);
    EXPECT_EQ(v.type(), DataType::kDouble);
    EXPECT_DOUBLE_EQ(v.as_double(), 9.9);
}

} // namespace
} // namespace pl::sstv2::types
