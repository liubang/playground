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
// Created: 2026/04/25 09:26

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/memory_source.h"
#include "cpp/pl/flux/runtime/runtime_builtin_package.h"
#include "cpp/pl/flux/runtime/runtime_eval.h"

namespace pl::flux::builtin {
namespace {

Value make_builtin_value(const std::string& name,
                         FunctionValue::BuiltinCallback fn,
                         std::string pipe_param_name = {}) {
    auto callable = std::make_shared<FunctionValue>();
    callable->kind = FunctionValue::Kind::Builtin;
    callable->name = name;
    callable->pipe_param_name = std::move(pipe_param_name);
    callable->builtin = std::move(fn);
    return Value::function(std::move(callable));
}

absl::StatusOr<const ObjectValue*> require_object_argument(const std::vector<Value>& args,
                                                           const std::string& name) {
    if (args.size() != 1 || args[0].type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " expects exactly one object argument"));
    }
    return &args[0].as_object();
}

absl::StatusOr<const Value*> require_object_property(const ObjectValue& object,
                                                     const std::string& object_name,
                                                     const std::string& property) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return absl::InvalidArgumentError(absl::StrCat(object_name, " requires `", property, "`"));
    }
    return value;
}

absl::StatusOr<const ArrayValue*> require_array_property(const ObjectValue& object,
                                                         const std::string& name,
                                                         const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Array) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be an array"));
    }
    return &(*value_or)->as_array();
}

absl::StatusOr<const FunctionValue*> require_function_property(const ObjectValue& object,
                                                               const std::string& name,
                                                               const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a function"));
    }
    return &(*value_or)->as_function();
}

absl::StatusOr<int64_t> value_as_int64(const Value& value,
                                       const std::string& name,
                                       const std::string& property) {
    if (value.type() == Value::Type::Int) {
        return value.as_int();
    }
    if (value.type() == Value::Type::UInt) {
        if (value.as_uint() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` is too large"));
        }
        return static_cast<int64_t>(value.as_uint());
    }
    return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be an integer"));
}

absl::StatusOr<int64_t> require_int_property(const ObjectValue& object,
                                             const std::string& name,
                                             const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    return value_as_int64(**value_or, name, property);
}

absl::StatusOr<int64_t> optional_int_property(const ObjectValue& object,
                                              const std::string& name,
                                              const std::string& property,
                                              int64_t default_value) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return default_value;
    }
    return value_as_int64(*value, name, property);
}

absl::StatusOr<bool> optional_bool_property(const ObjectValue& object,
                                            const std::string& name,
                                            const std::string& property,
                                            bool default_value) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return default_value;
    }
    if (value->type() != Value::Type::Bool) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a bool"));
    }
    return value->as_bool();
}

absl::StatusOr<std::string> optional_string_property(const ObjectValue& object,
                                                     const std::string& name,
                                                     const std::string& property,
                                                     std::string default_value) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return default_value;
    }
    if (value->type() != Value::Type::String) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a string"));
    }
    return value->as_string();
}

absl::StatusOr<size_t> normalize_array_index(int64_t index, size_t size, const std::string& name) {
    int64_t normalized = index;
    if (normalized < 0) {
        if (size > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            return absl::InvalidArgumentError(absl::StrCat(name, " array is too large"));
        }
        normalized += static_cast<int64_t>(size);
    }
    if (normalized < 0 || std::cmp_greater_equal(normalized, size)) {
        return absl::OutOfRangeError(absl::StrCat(name, " index out of range"));
    }
    return static_cast<size_t>(normalized);
}

int64_t clamp_slice_index(int64_t index, size_t size) {
    if (size > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
        return 0;
    }
    const auto signed_size = static_cast<int64_t>(size);
    int64_t normalized = index < 0 ? signed_size + index : index;
    normalized = std::max<int64_t>(0, normalized);
    normalized = std::min<int64_t>(signed_size, normalized);
    return normalized;
}

bool is_numeric_value(const Value& value) {
    return value.type() == Value::Type::Int || value.type() == Value::Type::UInt ||
           value.type() == Value::Type::Float;
}

double numeric_as_double(const Value& value) {
    if (value.type() == Value::Type::Float) {
        return value.as_float();
    }
    if (value.type() == Value::Type::UInt) {
        return static_cast<double>(value.as_uint());
    }
    return static_cast<double>(value.as_int());
}

absl::StatusOr<int> compare_array_values(const Value& lhs, const Value& rhs) {
    if (is_numeric_value(lhs) && is_numeric_value(rhs)) {
        const double left = numeric_as_double(lhs);
        const double right = numeric_as_double(rhs);
        if (left < right) {
            return -1;
        }
        if (left > right) {
            return 1;
        }
        return 0;
    }
    if (lhs.type() == Value::Type::String && rhs.type() == Value::Type::String) {
        return lhs.as_string().compare(rhs.as_string());
    }
    if (lhs.type() == Value::Type::Bool && rhs.type() == Value::Type::Bool) {
        return static_cast<int>(lhs.as_bool()) - static_cast<int>(rhs.as_bool());
    }
    return absl::InvalidArgumentError("array.sort supports only numeric, string, or bool values");
}

absl::StatusOr<std::vector<std::shared_ptr<ObjectValue>>> require_table_rows(
    const Value& value, const std::string& name) {
    if (value.type() != Value::Type::Array) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `rows` must be an array"));
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve(value.as_array().elements.size());
    for (const auto& item : value.as_array().elements) {
        if (item.type() != Value::Type::Object) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `rows` must contain only objects"));
        }
        rows.push_back(std::make_shared<ObjectValue>(item.as_object()));
    }
    return rows;
}

absl::StatusOr<std::vector<std::string>> parse_csv_record(const std::string& line,
                                                          const std::string& name) {
    if (line.find('"') == std::string::npos) {
        size_t field_count = 1;
        for (char ch : line) {
            if (ch == ',') {
                ++field_count;
            }
        }
        std::vector<std::string> fields;
        fields.reserve(field_count);
        size_t start = 0;
        while (true) {
            const size_t comma = line.find(',', start);
            if (comma == std::string::npos) {
                fields.emplace_back(line.substr(start));
                return fields;
            }
            fields.emplace_back(line.substr(start, comma - start));
            start = comma + 1;
        }
    }

    std::vector<std::string> fields;
    fields.reserve(8);
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(ch);
            }
            continue;
        }
        if (ch == ',') {
            fields.push_back(field);
            field.clear();
        } else if (ch == '"') {
            if (!field.empty()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " CSV quote must start a field"));
            }
            quoted = true;
        } else {
            field.push_back(ch);
        }
    }
    if (quoted) {
        return absl::InvalidArgumentError(absl::StrCat(name, " CSV has an unterminated quote"));
    }
    fields.push_back(field);
    return fields;
}

absl::StatusOr<Value> parse_raw_csv_table(const std::string& csv, const std::string& name) {
    std::istringstream input(csv);
    std::string line;
    std::vector<std::string> header;
    std::vector<std::shared_ptr<ObjectValue>> rows;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto fields_or = parse_csv_record(line, name);
        if (!fields_or.ok()) {
            return fields_or.status();
        }
        if (header.empty()) {
            header = std::move(*fields_or);
            continue;
        }
        if (fields_or->size() != header.size()) {
            return absl::InvalidArgumentError(absl::StrCat(
                name, " CSV row has ", fields_or->size(), " fields, expected ", header.size()));
        }
        std::vector<std::pair<std::string, Value>> properties;
        properties.reserve(header.size());
        for (size_t i = 0; i < header.size(); ++i) {
            properties.emplace_back(header[i], Value::string((*fields_or)[i]));
        }
        rows.push_back(std::make_shared<ObjectValue>(std::move(properties)));
    }
    if (header.empty()) {
        return absl::InvalidArgumentError(absl::StrCat(name, " CSV must include a header row"));
    }
    connector::CsvSource source(std::move(rows));
    return source.Scan({});
}

bool csv_bool(const std::string& value) {
    return value == "true" || value == "TRUE" || value == "True";
}

absl::StatusOr<Value> parse_annotated_csv_value(const std::string& raw,
                                                const std::string& datatype,
                                                const std::string& name) {
    if (raw.empty()) {
        return Value::null();
    }
    try {
        size_t consumed = 0;
        if (datatype == "string" || datatype == "base64Binary") {
            return Value::string(raw);
        }
        if (datatype == "long") {
            auto value = std::stoll(raw, &consumed, 10);
            if (consumed != raw.size()) {
                return absl::InvalidArgumentError(absl::StrCat(name, " invalid long value: ", raw));
            }
            return Value::integer(value);
        }
        if (datatype == "unsignedLong") {
            auto value = std::stoull(raw, &consumed, 10);
            if (consumed != raw.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " invalid unsignedLong value: ", raw));
            }
            return Value::uinteger(value);
        }
        if (datatype == "double") {
            auto value = std::stod(raw, &consumed);
            if (consumed != raw.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " invalid double value: ", raw));
            }
            return Value::floating(value);
        }
        if (datatype == "boolean" || datatype == "bool") {
            if (csv_bool(raw)) {
                return Value::boolean(true);
            }
            if (raw == "false" || raw == "FALSE" || raw == "False") {
                return Value::boolean(false);
            }
            return absl::InvalidArgumentError(absl::StrCat(name, " invalid boolean value: ", raw));
        }
        if (datatype == "dateTime:RFC3339" || datatype == "dateTime:RFC3339Nano") {
            return Value::time(raw);
        }
        if (datatype == "duration") {
            return Value::duration(raw);
        }
    } catch (const std::exception&) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " invalid ", datatype, " value: ", raw));
    }
    return Value::string(raw);
}

absl::StatusOr<Value> parse_annotated_csv_table(const std::string& csv, const std::string& name) {
    std::istringstream input(csv);
    std::string line;
    std::vector<std::string> datatypes;
    std::vector<bool> groups;
    std::vector<std::string> defaults;
    std::vector<std::string> header;
    std::vector<std::shared_ptr<ObjectValue>> rows;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto fields_or = parse_csv_record(line, name);
        if (!fields_or.ok()) {
            return fields_or.status();
        }
        auto fields = std::move(*fields_or);
        if (fields.empty()) {
            continue;
        }
        if (fields[0] == "#datatype") {
            datatypes = std::move(fields);
            groups.clear();
            defaults.clear();
            header.clear();
            continue;
        }
        if (fields[0] == "#group") {
            groups.clear();
            groups.reserve(fields.size());
            for (const auto& field : fields) {
                groups.push_back(csv_bool(field));
            }
            continue;
        }
        if (fields[0] == "#default") {
            defaults = std::move(fields);
            continue;
        }
        if (header.empty()) {
            header = std::move(fields);
            if (datatypes.size() != header.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name,
                                 " annotated CSV requires #datatype for each column: got ",
                                 datatypes.size(),
                                 ", expected ",
                                 header.size()));
            }
            if (groups.size() != header.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name,
                                 " annotated CSV requires #group for each column: got ",
                                 groups.size(),
                                 ", expected ",
                                 header.size()));
            }
            if (defaults.empty()) {
                defaults.resize(header.size());
            }
            if (defaults.size() != header.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name,
                                 " annotated CSV #default column count mismatch: got ",
                                 defaults.size(),
                                 ", expected ",
                                 header.size()));
            }
            continue;
        }
        if (fields.size() != header.size()) {
            return absl::InvalidArgumentError(absl::StrCat(
                name, " CSV row has ", fields.size(), " fields, expected ", header.size()));
        }
        std::vector<std::pair<std::string, Value>> properties;
        std::vector<std::pair<std::string, Value>> group_properties;
        for (size_t i = 0; i < header.size(); ++i) {
            if (header[i].empty()) {
                continue;
            }
            const std::string& raw = fields[i].empty() ? defaults[i] : fields[i];
            auto value_or = parse_annotated_csv_value(raw, datatypes[i], name);
            if (!value_or.ok()) {
                return value_or.status();
            }
            properties.emplace_back(header[i], *value_or);
            if (groups[i]) {
                group_properties.emplace_back(header[i], *value_or);
            }
        }
        if (!group_properties.empty()) {
            properties.emplace_back("_group", Value::object(std::move(group_properties)));
        }
        rows.push_back(std::make_shared<ObjectValue>(std::move(properties)));
    }
    if (header.empty()) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " annotated CSV must include a header row"));
    }
    connector::CsvSource source(std::move(rows));
    return source.Scan({});
}

absl::StatusOr<std::string> read_text_file(const std::string& path, const std::string& name) {
    std::ifstream file(path);
    if (!file) {
        return absl::NotFoundError(absl::StrCat(name, " cannot open file: ", path));
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

absl::StatusOr<Value> builtin_array_from(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.from");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto rows_or = require_object_property(**object_or, "array.from", "rows");
    if (!rows_or.ok()) {
        return rows_or.status();
    }
    auto parsed_rows_or = require_table_rows(**rows_or, "array.from");
    if (!parsed_rows_or.ok()) {
        return parsed_rows_or.status();
    }
    auto bucket_or = optional_string_property(**object_or, "array.from", "bucket", "array");
    if (!bucket_or.ok()) {
        return bucket_or.status();
    }
    connector::ArraySource source(*bucket_or, std::move(*parsed_rows_or));
    return source.Scan({});
}

absl::StatusOr<Value> builtin_array_concat(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.concat");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.concat", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto v_or = require_array_property(**object_or, "array.concat", "v");
    if (!v_or.ok()) {
        return v_or.status();
    }

    std::vector<Value> elements;
    elements.reserve((*arr_or)->elements.size() + (*v_or)->elements.size());
    elements.insert(elements.end(), (*arr_or)->elements.begin(), (*arr_or)->elements.end());
    elements.insert(elements.end(), (*v_or)->elements.begin(), (*v_or)->elements.end());
    return Value::array(std::move(elements));
}

absl::StatusOr<Value> builtin_array_filter(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.filter");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.filter", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.filter", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    std::vector<Value> filtered;
    filtered.reserve((*arr_or)->elements.size());
    for (const auto& element : (*arr_or)->elements) {
        auto keep_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!keep_or.ok()) {
            return keep_or.status();
        }
        if (keep_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("array.filter `fn` must return a boolean");
        }
        if (keep_or->as_bool()) {
            filtered.push_back(element);
        }
    }
    return Value::array(std::move(filtered));
}

absl::StatusOr<Value> builtin_array_map(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.map");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.map", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.map", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    std::vector<Value> mapped;
    mapped.reserve((*arr_or)->elements.size());
    for (const auto& element : (*arr_or)->elements) {
        auto mapped_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!mapped_or.ok()) {
            return mapped_or.status();
        }
        mapped.push_back(*mapped_or);
    }
    return Value::array(std::move(mapped));
}

absl::StatusOr<Value> builtin_array_contains(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.contains");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.contains", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto value_or = require_object_property(**object_or, "array.contains", "value");
    if (!value_or.ok()) {
        return value_or.status();
    }

    for (const auto& element : (*arr_or)->elements) {
        if (element == **value_or) {
            return Value::boolean(true);
        }
    }
    return Value::boolean(false);
}

absl::StatusOr<Value> builtin_array_reduce(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.reduce");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.reduce", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.reduce", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    auto identity_or = require_object_property(**object_or, "array.reduce", "identity");
    if (!identity_or.ok()) {
        return identity_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    Value accumulator = **identity_or;
    for (const auto& element : (*arr_or)->elements) {
        auto next_or = ExpressionEvaluator::Invoke(fn, {element, accumulator});
        if (!next_or.ok()) {
            return next_or.status();
        }
        accumulator = *next_or;
    }
    return accumulator;
}

absl::StatusOr<Value> builtin_array_any(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.any");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.any", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.any", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    for (const auto& element : (*arr_or)->elements) {
        auto matched_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!matched_or.ok()) {
            return matched_or.status();
        }
        if (matched_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("array.any `fn` must return a boolean");
        }
        if (matched_or->as_bool()) {
            return Value::boolean(true);
        }
    }
    return Value::boolean(false);
}

absl::StatusOr<Value> builtin_array_all(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.all");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.all", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.all", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    for (const auto& element : (*arr_or)->elements) {
        auto matched_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!matched_or.ok()) {
            return matched_or.status();
        }
        if (matched_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("array.all `fn` must return a boolean");
        }
        if (!matched_or->as_bool()) {
            return Value::boolean(false);
        }
    }
    return Value::boolean(true);
}

absl::StatusOr<Value> builtin_array_range(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.range");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto start_or = require_int_property(**object_or, "array.range", "start");
    if (!start_or.ok()) {
        return start_or.status();
    }
    auto stop_or = require_int_property(**object_or, "array.range", "stop");
    if (!stop_or.ok()) {
        return stop_or.status();
    }
    auto step_or = optional_int_property(**object_or, "array.range", "step", 1);
    if (!step_or.ok()) {
        return step_or.status();
    }
    if (*step_or == 0) {
        return absl::InvalidArgumentError("array.range `step` must not be zero");
    }

    std::vector<Value> elements;
    for (int64_t value = *start_or; *step_or > 0 ? value < *stop_or : value > *stop_or;
         value += *step_or) {
        elements.push_back(Value::integer(value));
    }
    return Value::array(std::move(elements));
}

absl::StatusOr<Value> builtin_array_repeat(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.repeat");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto value_or = require_object_property(**object_or, "array.repeat", "value");
    if (!value_or.ok()) {
        return value_or.status();
    }
    auto n_or = require_int_property(**object_or, "array.repeat", "n");
    if (!n_or.ok()) {
        return n_or.status();
    }
    if (*n_or < 0) {
        return absl::InvalidArgumentError("array.repeat `n` must be non-negative");
    }
    return Value::array(std::vector<Value>(static_cast<size_t>(*n_or), **value_or));
}

absl::StatusOr<Value> builtin_array_length(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.length");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.length", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    return Value::integer(static_cast<int64_t>((*arr_or)->elements.size()));
}

absl::StatusOr<Value> builtin_array_get(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.get");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.get", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto index_or = require_int_property(**object_or, "array.get", "index");
    if (!index_or.ok()) {
        return index_or.status();
    }
    auto normalized_or = normalize_array_index(*index_or, (*arr_or)->elements.size(), "array.get");
    if (!normalized_or.ok()) {
        const Value* default_value = (*object_or)->lookup("default");
        if (default_value != nullptr) {
            return *default_value;
        }
        return normalized_or.status();
    }
    return (*arr_or)->elements[*normalized_or];
}

absl::StatusOr<Value> builtin_array_slice(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.slice");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.slice", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto start_or = require_int_property(**object_or, "array.slice", "start");
    if (!start_or.ok()) {
        return start_or.status();
    }
    auto end_or = optional_int_property(
        **object_or, "array.slice", "end", static_cast<int64_t>((*arr_or)->elements.size()));
    if (!end_or.ok()) {
        return end_or.status();
    }
    const int64_t start = clamp_slice_index(*start_or, (*arr_or)->elements.size());
    const int64_t end = clamp_slice_index(*end_or, (*arr_or)->elements.size());
    if (end <= start) {
        return Value::array({});
    }
    return Value::array(
        std::vector<Value>((*arr_or)->elements.begin() + start, (*arr_or)->elements.begin() + end));
}

absl::StatusOr<Value> builtin_array_sort(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.sort");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.sort", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto desc_or = optional_bool_property(**object_or, "array.sort", "desc", false);
    if (!desc_or.ok()) {
        return desc_or.status();
    }

    std::vector<Value> sorted = (*arr_or)->elements;
    for (size_t i = 1; i < sorted.size(); ++i) {
        Value current = sorted[i];
        size_t j = i;
        while (j > 0) {
            auto cmp_or = compare_array_values(sorted[j - 1], current);
            if (!cmp_or.ok()) {
                return cmp_or.status();
            }
            const bool should_shift = *desc_or ? *cmp_or < 0 : *cmp_or > 0;
            if (!should_shift) {
                break;
            }
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = current;
    }
    return Value::array(std::move(sorted));
}

absl::StatusOr<Value> builtin_array_flat_map(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.flatMap");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.flatMap", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.flatMap", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    std::vector<Value> flattened;
    for (const auto& element : (*arr_or)->elements) {
        auto mapped_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!mapped_or.ok()) {
            return mapped_or.status();
        }
        if (mapped_or->type() != Value::Type::Array) {
            return absl::InvalidArgumentError("array.flatMap `fn` must return an array");
        }
        const auto& mapped = mapped_or->as_array().elements;
        flattened.insert(flattened.end(), mapped.begin(), mapped.end());
    }
    return Value::array(std::move(flattened));
}

absl::StatusOr<Value> builtin_array_find(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.find");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.find", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.find", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    for (const auto& element : (*arr_or)->elements) {
        auto matched_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!matched_or.ok()) {
            return matched_or.status();
        }
        if (matched_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("array.find `fn` must return a boolean");
        }
        if (matched_or->as_bool()) {
            return element;
        }
    }
    const Value* default_value = (*object_or)->lookup("default");
    return default_value == nullptr ? Value::null() : *default_value;
}

absl::StatusOr<Value> builtin_array_find_index(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.findIndex");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.findIndex", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.findIndex", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    for (size_t i = 0; i < (*arr_or)->elements.size(); ++i) {
        auto matched_or = ExpressionEvaluator::Invoke(fn, {(*arr_or)->elements[i]});
        if (!matched_or.ok()) {
            return matched_or.status();
        }
        if (matched_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("array.findIndex `fn` must return a boolean");
        }
        if (matched_or->as_bool()) {
            return Value::integer(static_cast<int64_t>(i));
        }
    }
    return Value::integer(-1);
}

absl::StatusOr<Value> builtin_array_take(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.take");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.take", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto n_or = require_int_property(**object_or, "array.take", "n");
    if (!n_or.ok()) {
        return n_or.status();
    }
    const auto count =
        *n_or <= 0 ? 0 : std::min(static_cast<size_t>(*n_or), (*arr_or)->elements.size());
    return Value::array(
        std::vector<Value>((*arr_or)->elements.begin(),
                           std::next((*arr_or)->elements.begin(), static_cast<int64_t>(count))));
}

absl::StatusOr<Value> builtin_array_drop(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.drop");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.drop", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto n_or = require_int_property(**object_or, "array.drop", "n");
    if (!n_or.ok()) {
        return n_or.status();
    }
    const auto count =
        *n_or <= 0 ? 0 : std::min(static_cast<size_t>(*n_or), (*arr_or)->elements.size());
    return Value::array(
        std::vector<Value>(std::next((*arr_or)->elements.begin(), static_cast<int64_t>(count)),
                           (*arr_or)->elements.end()));
}

absl::StatusOr<Value> builtin_array_reverse(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.reverse");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.reverse", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    std::vector<Value> reversed = (*arr_or)->elements;
    std::ranges::reverse(reversed);
    return Value::array(std::move(reversed));
}

absl::StatusOr<Value> builtin_array_unique(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.unique");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.unique", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    std::vector<Value> unique;
    for (const auto& element : (*arr_or)->elements) {
        if (std::ranges::find(unique, element) == unique.end()) {
            unique.push_back(element);
        }
    }
    return Value::array(std::move(unique));
}

absl::StatusOr<Value> builtin_array_unfold(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.unfold");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto seed_or = require_object_property(**object_or, "array.unfold", "seed");
    if (!seed_or.ok()) {
        return seed_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.unfold", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    auto limit_or = optional_int_property(**object_or, "array.unfold", "limit", 10000);
    if (!limit_or.ok()) {
        return limit_or.status();
    }
    if (*limit_or < 0) {
        return absl::InvalidArgumentError("array.unfold `limit` must be non-negative");
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    Value state = **seed_or;
    std::vector<Value> values;
    for (int64_t i = 0; i < *limit_or; ++i) {
        auto next_or = ExpressionEvaluator::Invoke(fn, {state});
        if (!next_or.ok()) {
            return next_or.status();
        }
        if (next_or->type() != Value::Type::Object) {
            return absl::InvalidArgumentError("array.unfold `fn` must return an object");
        }
        const auto& next = next_or->as_object();
        const Value* done = next.lookup("done");
        if (done != nullptr) {
            if (done->type() != Value::Type::Bool) {
                return absl::InvalidArgumentError("array.unfold `done` must be a bool");
            }
            if (done->as_bool()) {
                return Value::array(std::move(values));
            }
        }
        const Value* value = next.lookup("value");
        const Value* next_state = next.lookup("state");
        if (value == nullptr || next_state == nullptr) {
            return absl::InvalidArgumentError(
                "array.unfold `fn` must return `value` and `state` before done");
        }
        values.push_back(*value);
        state = *next_state;
    }
    return absl::InvalidArgumentError("array.unfold reached `limit` before done");
}

absl::StatusOr<Value> builtin_array_scan(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.scan");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.scan", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto identity_or = require_object_property(**object_or, "array.scan", "identity");
    if (!identity_or.ok()) {
        return identity_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.scan", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    Value accumulator = **identity_or;
    std::vector<Value> values;
    values.reserve((*arr_or)->elements.size());
    for (const auto& element : (*arr_or)->elements) {
        auto next_or = ExpressionEvaluator::Invoke(fn, {element, accumulator});
        if (!next_or.ok()) {
            return next_or.status();
        }
        accumulator = *next_or;
        values.push_back(accumulator);
    }
    return Value::array(std::move(values));
}

absl::StatusOr<Value> builtin_array_zip(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.zip");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto left_or = require_array_property(**object_or, "array.zip", "left");
    if (!left_or.ok()) {
        return left_or.status();
    }
    auto right_or = require_array_property(**object_or, "array.zip", "right");
    if (!right_or.ok()) {
        return right_or.status();
    }
    const size_t size = std::min((*left_or)->elements.size(), (*right_or)->elements.size());
    std::vector<Value> values;
    values.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        values.push_back(Value::object({
            {"left", (*left_or)->elements[i]},
            {"right", (*right_or)->elements[i]},
        }));
    }
    return Value::array(std::move(values));
}

absl::StatusOr<Value> builtin_array_enumerate(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.enumerate");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.enumerate", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    std::vector<Value> values;
    values.reserve((*arr_or)->elements.size());
    for (size_t i = 0; i < (*arr_or)->elements.size(); ++i) {
        values.push_back(Value::object({
            {"index", Value::integer(static_cast<int64_t>(i))},
            {"value", (*arr_or)->elements[i]},
        }));
    }
    return Value::array(std::move(values));
}

absl::StatusOr<Value> builtin_csv_from(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "csv.from");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto mode_or = optional_string_property(**object_or, "csv.from", "mode", "annotations");
    if (!mode_or.ok()) {
        return mode_or.status();
    }
    if (*mode_or != "raw" && *mode_or != "annotations") {
        return absl::InvalidArgumentError(R"(csv.from `mode` must be "raw" or "annotations")");
    }

    const Value* csv_value = (*object_or)->lookup("csv");
    const Value* file_value = (*object_or)->lookup("file");
    if ((csv_value == nullptr) == (file_value == nullptr)) {
        return absl::InvalidArgumentError("csv.from requires exactly one of `csv` or `file`");
    }
    if (csv_value != nullptr) {
        if (csv_value->type() != Value::Type::String) {
            return absl::InvalidArgumentError("csv.from `csv` must be a string");
        }
        if (*mode_or == "raw") {
            return parse_raw_csv_table(csv_value->as_string(), "csv.from");
        }
        return parse_annotated_csv_table(csv_value->as_string(), "csv.from");
    }
    if (file_value->type() != Value::Type::String) {
        return absl::InvalidArgumentError("csv.from `file` must be a string");
    }
    auto csv_or = read_text_file(file_value->as_string(), "csv.from");
    if (!csv_or.ok()) {
        return csv_or.status();
    }
    if (*mode_or == "raw") {
        return parse_raw_csv_table(*csv_or, "csv.from");
    }
    return parse_annotated_csv_table(*csv_or, "csv.from");
}

Value make_array_package() {
    return Value::object({
        {"path", Value::string("array")},
        {"from", make_builtin_value("array.from", builtin_array_from)},
        {"concat", make_builtin_value("array.concat", builtin_array_concat, "arr")},
        {"filter", make_builtin_value("array.filter", builtin_array_filter, "arr")},
        {"map", make_builtin_value("array.map", builtin_array_map, "arr")},
        {"contains", make_builtin_value("array.contains", builtin_array_contains, "arr")},
        {"reduce", make_builtin_value("array.reduce", builtin_array_reduce, "arr")},
        {"any", make_builtin_value("array.any", builtin_array_any, "arr")},
        {"all", make_builtin_value("array.all", builtin_array_all, "arr")},
        {"range", make_builtin_value("array.range", builtin_array_range)},
        {"repeat", make_builtin_value("array.repeat", builtin_array_repeat)},
        {"length", make_builtin_value("array.length", builtin_array_length, "arr")},
        {"get", make_builtin_value("array.get", builtin_array_get, "arr")},
        {"slice", make_builtin_value("array.slice", builtin_array_slice, "arr")},
        {"sort", make_builtin_value("array.sort", builtin_array_sort, "arr")},
        {"flatMap", make_builtin_value("array.flatMap", builtin_array_flat_map, "arr")},
        {"find", make_builtin_value("array.find", builtin_array_find, "arr")},
        {"findIndex", make_builtin_value("array.findIndex", builtin_array_find_index, "arr")},
        {"take", make_builtin_value("array.take", builtin_array_take, "arr")},
        {"drop", make_builtin_value("array.drop", builtin_array_drop, "arr")},
        {"reverse", make_builtin_value("array.reverse", builtin_array_reverse, "arr")},
        {"unique", make_builtin_value("array.unique", builtin_array_unique, "arr")},
        {"unfold", make_builtin_value("array.unfold", builtin_array_unfold)},
        {"scan", make_builtin_value("array.scan", builtin_array_scan, "arr")},
        {"zip", make_builtin_value("array.zip", builtin_array_zip)},
        {"enumerate", make_builtin_value("array.enumerate", builtin_array_enumerate, "arr")},
    });
}

Value make_csv_package() {
    return Value::object({
        {"path", Value::string("csv")},
        {"from", make_builtin_value("csv.from", builtin_csv_from)},
    });
}

} // namespace

void RegisterTableStdlibPackages() {
    RegisterPackage("array", make_array_package);
    RegisterPackage("csv", make_csv_package);
}

} // namespace pl::flux::builtin
