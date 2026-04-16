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

#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"

namespace pl {

struct ArrayValue;
struct ObjectValue;
struct TableValue;
struct FunctionExpr;
class Environment;
struct FunctionValue;

struct DurationValue {
    std::string literal;

    [[nodiscard]] std::string string() const;
    auto operator<=>(const DurationValue&) const = default;
};

struct TimeValue {
    std::string literal;

    [[nodiscard]] std::string string() const;
    auto operator<=>(const TimeValue&) const = default;
};

struct RegexValue {
    std::string literal;

    [[nodiscard]] std::string string() const;
    auto operator<=>(const RegexValue&) const = default;
};

class Value {
public:
    enum class Type {
        Null,
        Bool,
        Int,
        UInt,
        Float,
        String,
        Duration,
        Time,
        Regex,
        Array,
        Object,
        Table,
        Function,
    };

    using Storage = std::variant<std::monostate,
                                 bool,
                                 int64_t,
                                 uint64_t,
                                 double,
                                 std::string,
                                 DurationValue,
                                 TimeValue,
                                 RegexValue,
                                 std::shared_ptr<ArrayValue>,
                                 std::shared_ptr<ObjectValue>,
                                 std::shared_ptr<TableValue>,
                                 std::shared_ptr<FunctionValue>>;

    Value();

    static Value null();
    static Value boolean(bool value);
    static Value integer(int64_t value);
    static Value uinteger(uint64_t value);
    static Value floating(double value);
    static Value string(std::string value);
    static Value duration(std::string literal);
    static Value time(std::string literal);
    static Value regex(std::string literal);
    static Value array(std::vector<Value> elements);
    static Value object(std::vector<std::pair<std::string, Value>> properties);
    static Value table(std::string bucket,
                       std::vector<std::shared_ptr<ObjectValue>> rows,
                       std::optional<std::string> range_start = std::nullopt,
                       std::optional<std::string> range_stop = std::nullopt,
                       std::optional<std::string> result_name = std::nullopt);
    static Value function(std::shared_ptr<FunctionValue> function);

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool is_null() const { return type_ == Type::Null; }

    [[nodiscard]] const bool& as_bool() const;
    [[nodiscard]] const int64_t& as_int() const;
    [[nodiscard]] const uint64_t& as_uint() const;
    [[nodiscard]] const double& as_float() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const DurationValue& as_duration() const;
    [[nodiscard]] const TimeValue& as_time() const;
    [[nodiscard]] const RegexValue& as_regex() const;
    [[nodiscard]] const ArrayValue& as_array() const;
    [[nodiscard]] const ObjectValue& as_object() const;
    [[nodiscard]] const TableValue& as_table() const;
    [[nodiscard]] const FunctionValue& as_function() const;

    [[nodiscard]] std::string string() const;

    friend bool operator==(const Value& lhs, const Value& rhs);
    friend bool operator!=(const Value& lhs, const Value& rhs) { return !(lhs == rhs); }

private:
    explicit Value(Type type, Storage storage);

    Type type_;
    Storage storage_;
};

struct ArrayValue {
    std::vector<Value> elements;

    ArrayValue() = default;
    explicit ArrayValue(std::vector<Value> in_elements) : elements(std::move(in_elements)) {}

    [[nodiscard]] std::string string() const;
    bool operator==(const ArrayValue& other) const;
};

struct ObjectValue {
    std::vector<std::pair<std::string, Value>> properties;

    ObjectValue() = default;
    explicit ObjectValue(std::vector<std::pair<std::string, Value>> in_properties)
        : properties(std::move(in_properties)) {}

    [[nodiscard]] std::string string() const;
    bool operator==(const ObjectValue& other) const;

    [[nodiscard]] const Value* lookup(const std::string& key) const;
};

struct TableValue {
    std::string bucket;
    std::vector<std::shared_ptr<ObjectValue>> rows;
    std::optional<std::string> range_start;
    std::optional<std::string> range_stop;
    std::optional<std::string> result_name;

    [[nodiscard]] std::string string() const;
    bool operator==(const TableValue& other) const;
};

struct FunctionValue {
    enum class Kind {
        User,
        Builtin,
    };
    using BuiltinCallback = std::function<absl::StatusOr<Value>(const std::vector<Value>&)>;

    Kind kind = Kind::Builtin;
    std::string name;
    std::string pipe_param_name;
    const FunctionExpr* user_function = nullptr;
    std::shared_ptr<Environment> closure;
    BuiltinCallback builtin;

    [[nodiscard]] std::string string() const;
};

} // namespace pl
