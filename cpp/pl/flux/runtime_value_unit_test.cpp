// Copyright (c) 2023 The Authors. All rights reserved.
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

#include "cpp/pl/flux/runtime_value.h"
#include <gtest/gtest.h>

namespace pl {
namespace {

TEST(RuntimeValueTest, CreatesScalarValues) {
    const auto null_value = Value::null();
    const auto bool_value = Value::boolean(true);
    const auto int_value = Value::integer(-3);
    const auto uint_value = Value::uinteger(42);
    const auto float_value = Value::floating(3.5);
    const auto string_value = Value::string("cpu");
    const auto duration_value = Value::duration("5m");
    const auto time_value = Value::time("2024-01-02T03:04:05Z");
    const auto regex_value = Value::regex("/cpu.*/");

    EXPECT_TRUE(null_value.is_null());
    EXPECT_EQ("null", null_value.string());
    EXPECT_TRUE(bool_value.as_bool());
    EXPECT_EQ("-3", int_value.string());
    EXPECT_EQ("42u", uint_value.string());
    EXPECT_EQ("3.5", float_value.string());
    EXPECT_EQ("\"cpu\"", string_value.string());
    EXPECT_EQ("5m", duration_value.string());
    EXPECT_EQ("2024-01-02T03:04:05Z", time_value.string());
    EXPECT_EQ("/cpu.*/", regex_value.string());
}

TEST(RuntimeValueTest, CreatesNestedArrayAndObjectValues) {
    const auto metrics = Value::array({
        Value::integer(1),
        Value::object({
            {"name", Value::string("cpu")},
            {"active", Value::boolean(true)},
        }),
    });

    ASSERT_EQ(Value::Type::Array, metrics.type());
    ASSERT_EQ(2, metrics.as_array().elements.size());
    EXPECT_EQ("[1, {name: \"cpu\", active: true}]", metrics.string());

    const auto* nested = metrics.as_array().elements[1].as_object().lookup("name");
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ("cpu", nested->as_string());
}

TEST(RuntimeValueTest, PreservesObjectPropertyOrderAndLookup) {
    const auto config = Value::object({
        {"name", Value::string("cpu-alert")},
        {"every", Value::duration("5m")},
        {"offset", Value::duration("30s")},
    });

    ASSERT_EQ(Value::Type::Object, config.type());
    EXPECT_EQ("{name: \"cpu-alert\", every: 5m, offset: 30s}", config.string());

    const auto* every = config.as_object().lookup("every");
    ASSERT_NE(every, nullptr);
    EXPECT_EQ("5m", every->string());
    EXPECT_EQ(nullptr, config.as_object().lookup("missing"));
}

TEST(RuntimeValueTest, SupportsDeepEquality) {
    const auto lhs = Value::object({
        {"sum", Value::floating(10.5)},
        {"count", Value::uinteger(3)},
        {"tags", Value::array({Value::string("cpu"), Value::string("host")})},
    });
    const auto rhs = Value::object({
        {"sum", Value::floating(10.5)},
        {"count", Value::uinteger(3)},
        {"tags", Value::array({Value::string("cpu"), Value::string("host")})},
    });
    const auto different = Value::object({
        {"sum", Value::floating(11.5)},
        {"count", Value::uinteger(3)},
        {"tags", Value::array({Value::string("cpu"), Value::string("host")})},
    });

    EXPECT_EQ(lhs, rhs);
    EXPECT_NE(lhs, different);
}

TEST(RuntimeValueTest, ThrowsOnInvalidTypedAccess) {
    const auto value = Value::string("cpu");
    EXPECT_THROW((void)value.as_int(), std::logic_error);
}

TEST(RuntimeValueTest, CreatesTableValuesWithMetadata) {
    auto row = std::make_shared<ObjectValue>(std::vector<std::pair<std::string, Value>>{
        {"_measurement", Value::string("cpu")},
        {"_value", Value::floating(91.5)},
    });
    const auto table =
        Value::table("telegraf", {row}, "2024-01-01T00:00:00Z", "2024-01-02T00:00:00Z");

    ASSERT_EQ(Value::Type::Table, table.type());
    EXPECT_EQ("telegraf", table.as_table().bucket);
    ASSERT_EQ(1, table.as_table().rows.size());
    ASSERT_NE(nullptr, table.as_table().rows[0]);
    EXPECT_EQ("\"cpu\"", table.as_table().rows[0]->lookup("_measurement")->string());
    EXPECT_EQ("<table bucket=\"telegraf\" rows=1 start=2024-01-01T00:00:00Z stop=2024-01-02T00:00:00Z>",
              table.string());
}

} // namespace
} // namespace pl
