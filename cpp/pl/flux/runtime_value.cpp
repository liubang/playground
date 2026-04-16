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

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pl {

namespace {

template <typename T>
const T& checked_get(const Value::Storage& storage, Value::Type actual, Value::Type expected) {
    if (actual != expected) {
        std::stringstream ss;
        ss << "unexpected value type";
        throw std::logic_error(ss.str());
    }
    return std::get<T>(storage);
}

std::string quote_string(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    out.push_back('"');
    return out;
}

} // namespace

std::string DurationValue::string() const { return literal; }

std::string TimeValue::string() const { return literal; }

std::string RegexValue::string() const { return literal; }

Value::Value() : type_(Type::Null), storage_(std::monostate{}) {}

Value::Value(Type type, Storage storage) : type_(type), storage_(std::move(storage)) {}

Value Value::null() { return Value(); }

Value Value::boolean(bool value) { return Value(Type::Bool, value); }

Value Value::integer(int64_t value) { return Value(Type::Int, value); }

Value Value::uinteger(uint64_t value) { return Value(Type::UInt, value); }

Value Value::floating(double value) { return Value(Type::Float, value); }

Value Value::string(std::string value) { return Value(Type::String, std::move(value)); }

Value Value::duration(std::string literal) {
    return Value(Type::Duration, DurationValue{std::move(literal)});
}

Value Value::time(std::string literal) { return Value(Type::Time, TimeValue{std::move(literal)}); }

Value Value::regex(std::string literal) {
    return Value(Type::Regex, RegexValue{std::move(literal)});
}

Value Value::array(std::vector<Value> elements) {
    return Value(Type::Array, std::make_shared<ArrayValue>(std::move(elements)));
}

Value Value::object(std::vector<std::pair<std::string, Value>> properties) {
    return Value(Type::Object, std::make_shared<ObjectValue>(std::move(properties)));
}

Value Value::table(std::string bucket,
                   std::vector<std::shared_ptr<ObjectValue>> rows,
                   std::optional<std::string> range_start,
                   std::optional<std::string> range_stop,
                   std::optional<std::string> result_name) {
    auto table = std::make_shared<TableValue>();
    table->bucket = std::move(bucket);
    table->rows = std::move(rows);
    table->range_start = std::move(range_start);
    table->range_stop = std::move(range_stop);
    table->result_name = std::move(result_name);
    return Value(Type::Table, std::move(table));
}

Value Value::function(std::shared_ptr<FunctionValue> function) {
    return Value(Type::Function, std::move(function));
}

const bool& Value::as_bool() const { return checked_get<bool>(storage_, type_, Type::Bool); }

const int64_t& Value::as_int() const { return checked_get<int64_t>(storage_, type_, Type::Int); }

const uint64_t& Value::as_uint() const {
    return checked_get<uint64_t>(storage_, type_, Type::UInt);
}

const double& Value::as_float() const { return checked_get<double>(storage_, type_, Type::Float); }

const std::string& Value::as_string() const {
    return checked_get<std::string>(storage_, type_, Type::String);
}

const DurationValue& Value::as_duration() const {
    return checked_get<DurationValue>(storage_, type_, Type::Duration);
}

const TimeValue& Value::as_time() const {
    return checked_get<TimeValue>(storage_, type_, Type::Time);
}

const RegexValue& Value::as_regex() const {
    return checked_get<RegexValue>(storage_, type_, Type::Regex);
}

const ArrayValue& Value::as_array() const {
    return *checked_get<std::shared_ptr<ArrayValue>>(storage_, type_, Type::Array);
}

const ObjectValue& Value::as_object() const {
    return *checked_get<std::shared_ptr<ObjectValue>>(storage_, type_, Type::Object);
}

const TableValue& Value::as_table() const {
    return *checked_get<std::shared_ptr<TableValue>>(storage_, type_, Type::Table);
}

const FunctionValue& Value::as_function() const {
    return *checked_get<std::shared_ptr<FunctionValue>>(storage_, type_, Type::Function);
}

std::string Value::string() const {
    switch (type_) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return as_bool() ? "true" : "false";
        case Type::Int:
            return std::to_string(as_int());
        case Type::UInt:
            return std::to_string(as_uint()) + "u";
        case Type::Float: {
            std::ostringstream oss;
            oss << std::setprecision(15) << as_float();
            return oss.str();
        }
        case Type::String:
            return quote_string(as_string());
        case Type::Duration:
            return as_duration().string();
        case Type::Time:
            return as_time().string();
        case Type::Regex:
            return as_regex().string();
        case Type::Array:
            return as_array().string();
        case Type::Object:
            return as_object().string();
        case Type::Table:
            return as_table().string();
        case Type::Function:
            return as_function().string();
    }
}

bool operator==(const Value& lhs, const Value& rhs) {
    if (lhs.type_ != rhs.type_) {
        return false;
    }
    switch (lhs.type_) {
        case Value::Type::Null:
            return true;
        case Value::Type::Bool:
            return lhs.as_bool() == rhs.as_bool();
        case Value::Type::Int:
            return lhs.as_int() == rhs.as_int();
        case Value::Type::UInt:
            return lhs.as_uint() == rhs.as_uint();
        case Value::Type::Float:
            return lhs.as_float() == rhs.as_float();
        case Value::Type::String:
            return lhs.as_string() == rhs.as_string();
        case Value::Type::Duration:
            return lhs.as_duration() == rhs.as_duration();
        case Value::Type::Time:
            return lhs.as_time() == rhs.as_time();
        case Value::Type::Regex:
            return lhs.as_regex() == rhs.as_regex();
        case Value::Type::Array:
            return lhs.as_array() == rhs.as_array();
        case Value::Type::Object:
            return lhs.as_object() == rhs.as_object();
        case Value::Type::Table:
            return lhs.as_table() == rhs.as_table();
        case Value::Type::Function:
            return &lhs.as_function() == &rhs.as_function();
    }
}

std::string ArrayValue::string() const {
    std::string out = "[";
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += elements[i].string();
    }
    out += "]";
    return out;
}

bool ArrayValue::operator==(const ArrayValue& other) const { return elements == other.elements; }

std::string ObjectValue::string() const {
    std::string out = "{";
    for (size_t i = 0; i < properties.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += properties[i].first + ": " + properties[i].second.string();
    }
    out += "}";
    return out;
}

bool ObjectValue::operator==(const ObjectValue& other) const {
    return properties == other.properties;
}

const Value* ObjectValue::lookup(const std::string& key) const {
    for (const auto& [name, value] : properties) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

std::string TableValue::string() const {
    std::ostringstream oss;
    oss << "<table bucket=" << quote_string(bucket) << " rows=" << rows.size();
    if (range_start.has_value()) {
        oss << " start=" << *range_start;
    }
    if (range_stop.has_value()) {
        oss << " stop=" << *range_stop;
    }
    oss << ">";
    return oss.str();
}

bool TableValue::operator==(const TableValue& other) const {
    if (bucket != other.bucket || range_start != other.range_start || range_stop != other.range_stop ||
        result_name != other.result_name || rows.size() != other.rows.size()) {
        return false;
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        if ((rows[i] == nullptr) != (other.rows[i] == nullptr)) {
            return false;
        }
        if (rows[i] != nullptr && *rows[i] != *other.rows[i]) {
            return false;
        }
    }
    return true;
}

std::string FunctionValue::string() const {
    return kind == Kind::Builtin ? "<builtin " + name + ">" : "<function " + name + ">";
}

} // namespace pl
