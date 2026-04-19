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

#include "cpp/pl/flux/runtime_builtin.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "cpp/pl/flux/runtime_eval.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <exception>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace pl {
namespace {

enum class NumericKind {
    UInt,
    Int,
    Float,
};

struct NumericSummary {
    NumericKind kind = NumericKind::UInt;
    double float_sum = 0.0;
    int64_t int_sum = 0;
    uint64_t uint_sum = 0;
};

absl::StatusOr<const ArrayValue*> require_array_argument(const std::vector<Value>& args,
                                                         const std::string& name) {
    if (args.size() != 1 || args[0].type() != Value::Type::Array) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " expects exactly one array argument"));
    }
    return &args[0].as_array();
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
                                                         const std::string& property);

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

absl::StatusOr<const TableValue*> require_table_property(const ObjectValue& object,
                                                         const std::string& name,
                                                         const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Table) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a table"));
    }
    return &(*value_or)->as_table();
}

absl::StatusOr<std::vector<const TableValue*>> require_table_array_property(
    const ObjectValue& object, const std::string& name, const std::string& property) {
    auto array_or = require_array_property(object, name, property);
    if (!array_or.ok()) {
        return array_or.status();
    }
    std::vector<const TableValue*> tables;
    tables.reserve((*array_or)->elements.size());
    for (const auto& item : (*array_or)->elements) {
        if (item.type() != Value::Type::Table) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must contain only tables"));
        }
        tables.push_back(&item.as_table());
    }
    return tables;
}

absl::StatusOr<std::vector<std::pair<std::string, const TableValue*>>>
require_named_table_object_property(const ObjectValue& object,
                                    const std::string& name,
                                    const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be an object of tables"));
    }
    std::vector<std::pair<std::string, const TableValue*>> tables;
    for (const auto& [table_name, value] : (*value_or)->as_object().properties) {
        if (value.type() != Value::Type::Table) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must contain only tables"));
        }
        tables.emplace_back(table_name, &value.as_table());
    }
    return tables;
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

std::optional<std::string> optional_literal_property(const ObjectValue& object,
                                                     const std::string& property) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return std::nullopt;
    }
    return value->string();
}

absl::StatusOr<int64_t> integer_property(const ObjectValue& object,
                                         const std::string& name,
                                         const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() == Value::Type::Int) {
        return (*value_or)->as_int();
    }
    if ((*value_or)->type() == Value::Type::UInt) {
        return static_cast<int64_t>((*value_or)->as_uint());
    }
    return absl::InvalidArgumentError(
        absl::StrCat(name, " `", property, "` must be an int or uint"));
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
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a boolean"));
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

absl::StatusOr<size_t> optional_index_property(const ObjectValue& object,
                                               const std::string& name,
                                               const std::string& property,
                                               size_t default_value) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return default_value;
    }
    if (value->type() == Value::Type::UInt) {
        return static_cast<size_t>(value->as_uint());
    }
    if (value->type() == Value::Type::Int && value->as_int() >= 0) {
        return static_cast<size_t>(value->as_int());
    }
    return absl::InvalidArgumentError(
        absl::StrCat(name, " `", property, "` must be a non-negative int or uint"));
}

absl::StatusOr<std::string> string_property(const ObjectValue& object,
                                            const std::string& name,
                                            const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::String) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a string"));
    }
    return (*value_or)->as_string();
}

absl::StatusOr<std::vector<std::string>> string_array_property(const ObjectValue& object,
                                                               const std::string& name,
                                                               const std::string& property) {
    auto array_or = require_array_property(object, name, property);
    if (!array_or.ok()) {
        return array_or.status();
    }
    std::vector<std::string> values;
    values.reserve((*array_or)->elements.size());
    for (const auto& item : (*array_or)->elements) {
        if (item.type() != Value::Type::String) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must contain only strings"));
        }
        values.push_back(item.as_string());
    }
    return values;
}

absl::StatusOr<std::vector<std::string>> optional_string_array_property(
    const ObjectValue& object,
    const std::string& name,
    const std::string& property,
    std::vector<std::string> default_value) {
    if (object.lookup(property) == nullptr) {
        return default_value;
    }
    return string_array_property(object, name, property);
}

absl::StatusOr<std::vector<std::pair<std::string, std::string>>> string_map_property(
    const ObjectValue& object, const std::string& name, const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be an object"));
    }
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto& [key, value] : (*value_or)->as_object().properties) {
        if (value.type() != Value::Type::String) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` values must be strings"));
        }
        values.emplace_back(key, value.as_string());
    }
    return values;
}

std::optional<std::string> row_time_literal(const ObjectValue& row) {
    const Value* time = row.lookup("_time");
    if (time == nullptr) {
        return std::nullopt;
    }
    if (time->type() == Value::Type::Time || time->type() == Value::Type::String) {
        return time->type() == Value::Type::Time ? time->as_time().literal : time->as_string();
    }
    return std::nullopt;
}

bool row_matches_time_bounds(const ObjectValue& row,
                             const std::optional<std::string>& start,
                             const std::optional<std::string>& stop) {
    if (!start.has_value() && !stop.has_value()) {
        return true;
    }
    auto row_time = row_time_literal(row);
    if (!row_time.has_value()) {
        return true;
    }
    if (start.has_value() && *row_time < *start) {
        return false;
    }
    if (stop.has_value() && *row_time > *stop) {
        return false;
    }
    return true;
}

std::shared_ptr<ObjectValue> clone_row_with_selected_columns(
    const ObjectValue& row, const std::vector<std::string>& columns, bool keep_columns) {
    std::vector<std::pair<std::string, Value>> props;
    for (const auto& [key, value] : row.properties) {
        const bool listed = std::find(columns.begin(), columns.end(), key) != columns.end();
        if ((keep_columns && listed) || (!keep_columns && !listed)) {
            props.emplace_back(key, value);
        }
    }
    return std::make_shared<ObjectValue>(std::move(props));
}

std::shared_ptr<ObjectValue> clone_row(const ObjectValue& row) {
    return std::make_shared<ObjectValue>(row);
}

Value object_with_upserted_property(const ObjectValue& object,
                                    const std::string& key,
                                    Value value) {
    auto props = object.properties;
    for (auto& [name, current] : props) {
        if (name == key) {
            current = std::move(value);
            return Value::object(std::move(props));
        }
    }
    props.emplace_back(key, std::move(value));
    return Value::object(std::move(props));
}

std::shared_ptr<ObjectValue> clone_row_with_group(const ObjectValue& row,
                                                  const std::vector<std::string>& columns) {
    std::vector<std::pair<std::string, Value>> group_props;
    for (const auto& column : columns) {
        const Value* value = row.lookup(column);
        if (value != nullptr) {
            group_props.emplace_back(column, *value);
        }
    }
    auto grouped =
        object_with_upserted_property(row, "_group", Value::object(std::move(group_props)));
    return std::make_shared<ObjectValue>(grouped.as_object());
}

double numeric_value(const Value& value) {
    switch (value.type()) {
        case Value::Type::Int:
            return static_cast<double>(value.as_int());
        case Value::Type::UInt:
            return static_cast<double>(value.as_uint());
        case Value::Type::Float:
            return value.as_float();
        default:
            return 0.0;
    }
}

bool is_numeric_value(const Value& value) {
    return value.type() == Value::Type::Int || value.type() == Value::Type::UInt ||
           value.type() == Value::Type::Float;
}

int compare_values(const Value* lhs, const Value* rhs) {
    if (lhs == nullptr && rhs == nullptr) {
        return 0;
    }
    if (lhs == nullptr) {
        return 1;
    }
    if (rhs == nullptr) {
        return -1;
    }
    if (is_numeric_value(*lhs) && is_numeric_value(*rhs)) {
        const auto left = numeric_value(*lhs);
        const auto right = numeric_value(*rhs);
        return left < right ? -1 : left > right ? 1 : 0;
    }
    const auto left = lhs->type() == Value::Type::String ? lhs->as_string() : lhs->string();
    const auto right = rhs->type() == Value::Type::String ? rhs->as_string() : rhs->string();
    return left < right ? -1 : left > right ? 1 : 0;
}

bool contains_string(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::optional<std::string> mapped_column_name(
    const std::vector<std::pair<std::string, std::string>>& mappings, const std::string& key) {
    for (const auto& [from, to] : mappings) {
        if (from == key) {
            return to;
        }
    }
    return std::nullopt;
}

std::string value_key_fragment(const Value& value) {
    if (value.type() == Value::Type::String) {
        return value.as_string();
    }
    if (value.type() == Value::Type::Time) {
        return value.as_time().literal;
    }
    return value.string();
}

std::string row_identity_key(const ObjectValue& row, const std::vector<std::string>& columns) {
    std::string key;
    for (const auto& column : columns) {
        absl::StrAppend(&key, column, "=");
        if (const Value* value = row.lookup(column); value != nullptr) {
            absl::StrAppend(&key, value_key_fragment(*value));
        } else {
            absl::StrAppend(&key, "<missing>");
        }
        key.push_back('\n');
    }
    return key;
}

std::string window_bucket_key(const std::optional<int64_t>& start_seconds,
                              const std::string& group_key) {
    return absl::StrCat(start_seconds.has_value() ? std::to_string(*start_seconds) : "<none>", "\n",
                        group_key);
}

std::string pivot_column_name(const ObjectValue& row, const std::vector<std::string>& columns) {
    std::string name;
    for (const auto& column : columns) {
        if (!name.empty()) {
            name.push_back('_');
        }
        if (const Value* value = row.lookup(column); value != nullptr) {
            absl::StrAppend(&name, value_key_fragment(*value));
        } else {
            absl::StrAppend(&name, "null");
        }
    }
    return name;
}

absl::StatusOr<std::vector<std::string>> parse_csv_record(const std::string& line,
                                                          const std::string& name) {
    std::vector<std::string> fields;
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
            return absl::InvalidArgumentError(absl::StrCat(name, " CSV row has ", fields_or->size(),
                                                           " fields, expected ", header.size()));
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
    return Value::table("csv", std::move(rows));
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
                    absl::StrCat(name, " annotated CSV requires #datatype for each column: got ",
                                 datatypes.size(), ", expected ", header.size()));
            }
            if (groups.size() != header.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " annotated CSV requires #group for each column: got ",
                                 groups.size(), ", expected ", header.size()));
            }
            if (defaults.empty()) {
                defaults.resize(header.size());
            }
            if (defaults.size() != header.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " annotated CSV #default column count mismatch: got ",
                                 defaults.size(), ", expected ", header.size()));
            }
            continue;
        }
        if (fields.size() != header.size()) {
            return absl::InvalidArgumentError(absl::StrCat(name, " CSV row has ", fields.size(),
                                                           " fields, expected ", header.size()));
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
    return Value::table("csv", std::move(rows));
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

std::string joined_property_name(const std::string& table_name, const std::string& column) {
    return table_name + "." + column;
}

std::shared_ptr<ObjectValue> join_rows(const std::string& left_name,
                                       const ObjectValue& left,
                                       const std::string& right_name,
                                       const ObjectValue& right,
                                       const std::vector<std::string>& on_columns) {
    std::vector<std::pair<std::string, Value>> props;
    for (const auto& column : on_columns) {
        const Value* value = left.lookup(column);
        if (value != nullptr) {
            props.emplace_back(column, *value);
        }
    }
    for (const auto& [key, value] : left.properties) {
        if (!contains_string(on_columns, key)) {
            props.emplace_back(joined_property_name(left_name, key), value);
        }
    }
    for (const auto& [key, value] : right.properties) {
        if (!contains_string(on_columns, key)) {
            props.emplace_back(joined_property_name(right_name, key), value);
        }
    }
    return std::make_shared<ObjectValue>(std::move(props));
}

bool parse_fixed_int(const std::string& text, size_t offset, size_t width, int* out) {
    if (offset + width > text.size()) {
        return false;
    }
    int value = 0;
    for (size_t i = 0; i < width; ++i) {
        const auto ch = text[offset + i];
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
        value = value * 10 + (ch - '0');
    }
    *out = value;
    return true;
}

int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= static_cast<int>(month <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const auto shifted_month =
        static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
    const unsigned doy = (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

std::optional<int64_t> parse_rfc3339_seconds(const std::string& literal) {
    absl::Time timestamp;
    std::string error;
    if (!absl::ParseTime(absl::RFC3339_full, literal, &timestamp, &error)) {
        return std::nullopt;
    }
    return absl::ToUnixSeconds(timestamp);
}

std::string format_rfc3339_seconds(int64_t seconds) {
    return absl::FormatTime("%Y-%m-%dT%H:%M:%SZ", absl::FromUnixSeconds(seconds),
                            absl::UTCTimeZone());
}

struct WindowDuration {
    enum class Kind {
        FixedSeconds,
        CalendarMonths,
    };

    Kind kind = Kind::FixedSeconds;
    int64_t seconds = 0;
    int64_t months = 0;
};

absl::StatusOr<WindowDuration> parse_window_duration(const Value& value,
                                                     const std::string& name,
                                                     const std::string& property,
                                                     bool allow_negative,
                                                     bool allow_zero);

struct WindowLocation {
    enum class Kind {
        Utc,
        FixedOffset,
        NamedZone,
    };

    Kind kind = Kind::Utc;
    int64_t fixed_offset_seconds = 0;
    std::string zone_name = "UTC";
    absl::TimeZone zone = absl::UTCTimeZone();
};

int64_t floor_div(int64_t lhs, int64_t rhs);

int64_t utc_seconds_from_civil(int year,
                               unsigned month,
                               unsigned day,
                               unsigned hour = 0,
                               unsigned minute = 0,
                               unsigned second = 0) {
    return days_from_civil(year, month, day) * 24 * 60 * 60 + static_cast<int64_t>(hour) * 60 * 60 +
           static_cast<int64_t>(minute) * 60 + static_cast<int64_t>(second);
}

absl::CivilSecond civil_second_from_utc_seconds(int64_t seconds) {
    return absl::ToCivilSecond(absl::FromUnixSeconds(seconds), absl::UTCTimeZone());
}

int64_t seconds_from_civil_second(const absl::CivilSecond& civil) {
    return utc_seconds_from_civil(
        civil.year(), static_cast<unsigned>(civil.month()), static_cast<unsigned>(civil.day()),
        static_cast<unsigned>(civil.hour()), static_cast<unsigned>(civil.minute()),
        static_cast<unsigned>(civil.second()));
}

absl::CivilSecond civil_second_in_location(int64_t seconds, const WindowLocation& location) {
    if (location.kind == WindowLocation::Kind::NamedZone) {
        return absl::ToCivilSecond(absl::FromUnixSeconds(seconds), location.zone);
    }
    return civil_second_from_utc_seconds(seconds + location.fixed_offset_seconds);
}

int64_t seconds_for_civil_in_location(const absl::CivilSecond& civil,
                                      const WindowLocation& location) {
    if (location.kind == WindowLocation::Kind::NamedZone) {
        const auto info = location.zone.At(civil);
        if (info.kind == absl::TimeZone::TimeInfo::SKIPPED) {
            return absl::ToUnixSeconds(info.trans);
        }
        return absl::ToUnixSeconds(info.pre);
    }
    return seconds_from_civil_second(civil) - location.fixed_offset_seconds;
}

int64_t month_index_for_seconds(int64_t seconds) {
    auto time = static_cast<std::time_t>(seconds);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    return static_cast<int64_t>(tm.tm_year + 1900) * 12 + tm.tm_mon;
}

int64_t seconds_for_month_index(int64_t month_index) {
    int64_t year = floor_div(month_index, 12);
    int64_t month = month_index - year * 12;
    return utc_seconds_from_civil(static_cast<int>(year), static_cast<unsigned>(month + 1), 1);
}

int64_t advance_calendar_months(int64_t seconds, int64_t months) {
    return seconds_for_month_index(month_index_for_seconds(seconds) + months);
}

absl::CivilSecond add_months_to_civil_second(const absl::CivilSecond& civil, int64_t months) {
    const int64_t month_index =
        static_cast<int64_t>(civil.year()) * 12 + static_cast<int64_t>(civil.month()) - 1 + months;
    const int64_t year = floor_div(month_index, 12);
    const int64_t month = month_index - year * 12;
    return absl::CivilSecond(static_cast<int>(year), static_cast<int>(month + 1), civil.day(),
                             civil.hour(), civil.minute(), civil.second());
}

absl::StatusOr<WindowLocation> optional_window_location_property(const ObjectValue& object,
                                                                 const std::string& name) {
    const Value* location_value = object.lookup("location");
    if (location_value == nullptr) {
        return WindowLocation{};
    }
    if (location_value->type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `location` must be an object record"));
    }
    const auto& location = location_value->as_object();
    auto zone_or = optional_string_property(location, name, "zone", "UTC");
    if (!zone_or.ok()) {
        return zone_or.status();
    }
    int64_t fixed_offset_seconds = 0;
    if (const Value* offset_value = location.lookup("offset"); offset_value != nullptr) {
        auto offset_or = parse_window_duration(*offset_value, name, "location.offset", true, true);
        if (!offset_or.ok()) {
            return offset_or.status();
        }
        if (offset_or->kind != WindowDuration::Kind::FixedSeconds) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `location.offset` does not support calendar durations"));
        }
        fixed_offset_seconds = offset_or->seconds;
    }
    if (*zone_or == "UTC") {
        return WindowLocation{.kind = fixed_offset_seconds == 0 ? WindowLocation::Kind::Utc
                                                                : WindowLocation::Kind::FixedOffset,
                              .fixed_offset_seconds = fixed_offset_seconds,
                              .zone_name = *zone_or,
                              .zone = absl::UTCTimeZone()};
    }
    if (fixed_offset_seconds != 0) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `location` with a named zone must use offset 0s"));
    }
    absl::TimeZone zone;
    if (!absl::LoadTimeZone(*zone_or, &zone)) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " unknown timezone location: ", *zone_or));
    }
    return WindowLocation{.kind = WindowLocation::Kind::NamedZone,
                          .fixed_offset_seconds = 0,
                          .zone_name = *zone_or,
                          .zone = zone};
}

absl::StatusOr<WindowDuration> parse_window_duration(const Value& value,
                                                     const std::string& name,
                                                     const std::string& property,
                                                     bool allow_negative = false,
                                                     bool allow_zero = false) {
    std::string literal;
    if (value.type() == Value::Type::Duration) {
        literal = value.as_duration().literal;
    } else if (value.type() == Value::Type::String) {
        literal = value.as_string();
    } else {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a duration"));
    }

    int sign = 1;
    size_t index = 0;
    if (!literal.empty() && (literal[0] == '+' || literal[0] == '-')) {
        if (!allow_negative) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must be a positive duration"));
        }
        sign = literal[0] == '-' ? -1 : 1;
        index = 1;
    }

    if (index >= literal.size()) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a duration"));
    }
    int64_t fixed_total = 0;
    int64_t calendar_months = 0;
    bool saw_fixed_unit = false;
    bool saw_calendar_unit = false;
    while (index < literal.size()) {
        if (std::isdigit(static_cast<unsigned char>(literal[index])) == 0) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must be ",
                             allow_negative ? "a duration" : "a positive duration"));
        }
        int64_t amount = 0;
        while (index < literal.size() &&
               (std::isdigit(static_cast<unsigned char>(literal[index])) != 0)) {
            amount = amount * 10 + (literal[index] - '0');
            ++index;
        }
        const size_t unit_begin = index;
        while (index < literal.size() &&
               (std::isalpha(static_cast<unsigned char>(literal[index])) != 0)) {
            ++index;
        }
        const auto unit = literal.substr(unit_begin, index - unit_begin);
        if (unit == "s") {
            fixed_total += amount;
            saw_fixed_unit = true;
        } else if (unit == "m") {
            fixed_total += amount * 60;
            saw_fixed_unit = true;
        } else if (unit == "h") {
            fixed_total += amount * 60 * 60;
            saw_fixed_unit = true;
        } else if (unit == "d") {
            fixed_total += amount * 24 * 60 * 60;
            saw_fixed_unit = true;
        } else if (unit == "w") {
            fixed_total += amount * 7 * 24 * 60 * 60;
            saw_fixed_unit = true;
        } else if (unit == "mo") {
            calendar_months += amount;
            saw_calendar_unit = true;
        } else if (unit == "y") {
            calendar_months += amount * 12;
            saw_calendar_unit = true;
        } else {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` supports s, m, h, d, w, mo, and y units"));
        }
        if (saw_fixed_unit && saw_calendar_unit) {
            return absl::InvalidArgumentError(absl::StrCat(
                name, " `", property, "` cannot mix calendar units with fixed-duration units"));
        }
    }
    fixed_total *= sign;
    calendar_months *= sign;
    if ((!allow_zero && fixed_total == 0 && calendar_months == 0) ||
        (!allow_negative && fixed_total < 0)) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a positive duration"));
    }
    if (!allow_negative && fixed_total <= 0 && calendar_months == 0) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a positive duration"));
    }
    if (saw_calendar_unit) {
        if ((!allow_zero && calendar_months == 0) || (!allow_negative && calendar_months < 0)) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must be a positive duration"));
        }
        return WindowDuration{
            .kind = WindowDuration::Kind::CalendarMonths, .seconds = 0, .months = calendar_months};
    }
    return WindowDuration{
        .kind = WindowDuration::Kind::FixedSeconds, .seconds = fixed_total, .months = 0};
}

int64_t floor_div(int64_t lhs, int64_t rhs) {
    int64_t quotient = lhs / rhs;
    int64_t remainder = lhs % rhs;
    if (remainder != 0 && ((remainder > 0) != (rhs > 0))) {
        --quotient;
    }
    return quotient;
}

bool window_duration_is_negative(const WindowDuration& duration) {
    return duration.kind == WindowDuration::Kind::FixedSeconds ? duration.seconds < 0
                                                               : duration.months < 0;
}

bool window_duration_is_zero(const WindowDuration& duration) {
    return duration.kind == WindowDuration::Kind::FixedSeconds ? duration.seconds == 0
                                                               : duration.months == 0;
}

WindowDuration negate_window_duration(const WindowDuration& duration) {
    if (duration.kind == WindowDuration::Kind::FixedSeconds) {
        return WindowDuration{
            .kind = WindowDuration::Kind::FixedSeconds, .seconds = -duration.seconds, .months = 0};
    }
    return WindowDuration{
        .kind = WindowDuration::Kind::CalendarMonths, .seconds = 0, .months = -duration.months};
}

std::optional<int64_t> add_window_duration_to_time(int64_t seconds,
                                                   const WindowDuration& duration,
                                                   const WindowLocation& location) {
    if (duration.kind == WindowDuration::Kind::FixedSeconds) {
        if (location.kind == WindowLocation::Kind::Utc ||
            location.kind == WindowLocation::Kind::FixedOffset) {
            return seconds + duration.seconds;
        }
        const auto local = civil_second_in_location(seconds, location);
        return seconds_for_civil_in_location(
            civil_second_from_utc_seconds(seconds_from_civil_second(local) + duration.seconds),
            location);
    }
    const auto local = civil_second_in_location(seconds, location);
    return seconds_for_civil_in_location(add_months_to_civil_second(local, duration.months),
                                         location);
}

std::optional<int64_t> aggregate_window_start_for_time(int64_t seconds,
                                                       const WindowDuration& every,
                                                       const WindowDuration& offset,
                                                       const WindowLocation& location) {
    if (every.kind == WindowDuration::Kind::FixedSeconds) {
        if (offset.kind != WindowDuration::Kind::FixedSeconds) {
            return std::nullopt;
        }
        const int64_t offset_seconds = offset.seconds;
        if (location.kind == WindowLocation::Kind::Utc ||
            location.kind == WindowLocation::Kind::FixedOffset) {
            const int64_t anchor = offset_seconds - location.fixed_offset_seconds;
            return floor_div(seconds - anchor, every.seconds) * every.seconds + anchor;
        }
        const auto local = civil_second_in_location(seconds, location);
        const int64_t local_seconds = seconds_from_civil_second(local);
        const int64_t start_local_seconds =
            floor_div(local_seconds - offset_seconds, every.seconds) * every.seconds +
            offset_seconds;
        return seconds_for_civil_in_location(civil_second_from_utc_seconds(start_local_seconds),
                                             location);
    }
    if (window_duration_is_zero(offset)) {
        const auto local = civil_second_in_location(seconds, location);
        const int64_t month_index =
            static_cast<int64_t>(local.year()) * 12 + static_cast<int64_t>(local.month()) - 1;
        const int64_t start_index = floor_div(month_index, every.months) * every.months;
        const int64_t year = floor_div(start_index, 12);
        const int64_t month = start_index - year * 12;
        return seconds_for_civil_in_location(
            absl::CivilSecond(static_cast<int>(year), static_cast<int>(month + 1), 1, 0, 0, 0),
            location);
    }
    auto anchor_or = add_window_duration_to_time(0, offset, location);
    if (!anchor_or.has_value()) {
        return std::nullopt;
    }
    const auto local = civil_second_in_location(seconds, location);
    const auto anchor_local = civil_second_in_location(*anchor_or, location);
    const int64_t month_index =
        static_cast<int64_t>(local.year()) * 12 + static_cast<int64_t>(local.month()) - 1;
    const int64_t anchor_month_index = static_cast<int64_t>(anchor_local.year()) * 12 +
                                       static_cast<int64_t>(anchor_local.month()) - 1;
    int64_t step = floor_div(month_index - anchor_month_index, every.months);
    auto candidate_or =
        add_window_duration_to_time(*anchor_or,
                                    WindowDuration{.kind = WindowDuration::Kind::CalendarMonths,
                                                   .seconds = 0,
                                                   .months = step * every.months},
                                    location);
    if (!candidate_or.has_value()) {
        return std::nullopt;
    }
    while (*candidate_or > seconds) {
        auto previous_or =
            add_window_duration_to_time(*candidate_or, negate_window_duration(every), location);
        if (!previous_or.has_value() || *previous_or >= *candidate_or) {
            break;
        }
        candidate_or = previous_or;
    }
    while (true) {
        auto next_or = add_window_duration_to_time(*candidate_or, every, location);
        if (!next_or.has_value() || *next_or <= *candidate_or || *next_or > seconds) {
            break;
        }
        candidate_or = next_or;
    }
    return candidate_or;
}

struct WindowBounds {
    int64_t start_seconds = 0;
    int64_t stop_seconds = 0;
    int64_t lower_seconds = 0;
    int64_t upper_seconds = 0;
};

std::optional<WindowBounds> aggregate_window_bounds_for_start(int64_t start_seconds,
                                                              const WindowDuration& period,
                                                              const WindowLocation& location) {
    auto stop_or = add_window_duration_to_time(start_seconds, period, location);
    if (!stop_or.has_value()) {
        return std::nullopt;
    }
    return WindowBounds{.start_seconds = start_seconds,
                        .stop_seconds = *stop_or,
                        .lower_seconds = std::min(start_seconds, *stop_or),
                        .upper_seconds = std::max(start_seconds, *stop_or)};
}

bool aggregate_window_contains_time(int64_t seconds, const WindowBounds& bounds) {
    return seconds >= bounds.lower_seconds && seconds < bounds.upper_seconds;
}

bool aggregate_window_intersects_range(const WindowBounds& bounds,
                                       int64_t range_start_seconds,
                                       int64_t range_stop_seconds) {
    return bounds.upper_seconds > range_start_seconds && bounds.lower_seconds < range_stop_seconds;
}

bool aggregate_window_is_within_range(const WindowBounds& bounds,
                                      int64_t range_start_seconds,
                                      int64_t range_stop_seconds) {
    return bounds.lower_seconds >= range_start_seconds &&
           bounds.upper_seconds <= range_stop_seconds;
}

bool aggregate_window_fn_drops_empty(const FunctionValue& fn) {
    if (fn.kind != FunctionValue::Kind::Builtin) {
        return false;
    }
    return fn.name == "first" || fn.name == "last";
}

std::string group_key_for_row(const ObjectValue& row) {
    const Value* group = row.lookup("_group");
    return group == nullptr ? "" : group->string();
}

std::unordered_set<std::string> aggregate_window_allowed_columns(
    const ObjectValue& row,
    const std::string& aggregate_column,
    const std::string& time_dst_column) {
    std::unordered_set<std::string> allowed = {"_group", "_start", "_stop", aggregate_column,
                                               time_dst_column};
    const Value* group = row.lookup("_group");
    if (group != nullptr && group->type() == Value::Type::Object) {
        for (const auto& [name, value] : group->as_object().properties) {
            (void)value;
            allowed.insert(name);
        }
    }
    return allowed;
}

std::shared_ptr<ObjectValue> aggregate_window_base_row(const ObjectValue& row,
                                                       const std::string& aggregate_column,
                                                       const std::string& time_dst_column) {
    std::vector<std::pair<std::string, Value>> properties;
    const auto allowed = aggregate_window_allowed_columns(row, aggregate_column, time_dst_column);
    properties.reserve(allowed.size());
    for (const auto& [name, value] : row.properties) {
        if (allowed.find(name) == allowed.end()) {
            continue;
        }
        properties.emplace_back(name, value);
    }
    return std::make_shared<ObjectValue>(std::move(properties));
}

std::optional<int64_t> aggregate_window_time_src_seconds(
    const std::shared_ptr<ObjectValue>& first_row,
    int64_t window_start,
    int64_t window_stop,
    const std::string& time_src_column) {
    if (time_src_column == "_start") {
        return window_start;
    }
    if (time_src_column == "_stop") {
        return window_stop;
    }
    if (first_row == nullptr) {
        return std::nullopt;
    }
    if (const Value* source = first_row->lookup(time_src_column); source != nullptr) {
        if (source->type() == Value::Type::Time) {
            return parse_rfc3339_seconds(source->as_time().literal);
        }
        if (source->type() == Value::Type::String) {
            return parse_rfc3339_seconds(source->as_string());
        }
    }
    return std::nullopt;
}

std::vector<std::string> visible_columns_in_order(const TableValue& table) {
    std::vector<std::string> columns;
    std::unordered_set<std::string> seen;
    for (const auto& row : table.rows) {
        if (row == nullptr) {
            continue;
        }
        for (const auto& [name, value] : row->properties) {
            (void)value;
            if (name == "_group") {
                continue;
            }
            if (seen.insert(name).second) {
                columns.push_back(name);
            }
        }
    }
    return columns;
}

std::vector<std::string> group_columns_in_order(const TableValue& table) {
    std::vector<std::string> columns;
    std::unordered_set<std::string> seen;
    for (const auto& row : table.rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* group = row->lookup("_group");
        if (group == nullptr || group->type() != Value::Type::Object) {
            continue;
        }
        for (const auto& [name, value] : group->as_object().properties) {
            (void)value;
            if (seen.insert(name).second) {
                columns.push_back(name);
            }
        }
    }
    return columns;
}

absl::StatusOr<std::vector<std::shared_ptr<ObjectValue>>> filter_rows_by_function(
    const TableValue& table, const Value& predicate, const std::string& name) {
    if (predicate.type() != Value::Type::Function) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `fn` must be a function"));
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    for (const auto& row : table.rows) {
        if (row == nullptr) {
            continue;
        }
        auto keep_or = ExpressionEvaluator::Invoke(predicate, {Value::object(row->properties)});
        if (!keep_or.ok()) {
            return keep_or.status();
        }
        if (keep_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError(absl::StrCat(name, " `fn` must return a boolean"));
        }
        if (keep_or->as_bool()) {
            rows.push_back(row);
        }
    }
    return rows;
}

struct AggregateWindowBucket {
    std::optional<int64_t> start_seconds;
    std::string group_key;
    std::shared_ptr<ObjectValue> first_row;
    std::vector<Value> values;
};

struct AggregateWindowGroupSpan {
    std::string group_key;
    std::shared_ptr<ObjectValue> template_row;
    int64_t min_time_seconds = 0;
    int64_t max_time_exclusive_seconds = 0;
};

absl::StatusOr<Value> invoke_window_aggregate(const FunctionValue& fn,
                                              const std::vector<Value>& values) {
    if (fn.kind == FunctionValue::Kind::Builtin && fn.name == "count") {
        return Value::integer(static_cast<int64_t>(values.size()));
    }
    if (fn.kind == FunctionValue::Kind::Builtin && fn.name == "first") {
        if (values.empty()) {
            return Value::null();
        }
        return values.front();
    }
    if (fn.kind == FunctionValue::Kind::Builtin && fn.name == "last") {
        if (values.empty()) {
            return Value::null();
        }
        return values.back();
    }
    return ExpressionEvaluator::Invoke(Value::function(std::make_shared<FunctionValue>(fn)),
                                       {Value::array(values)});
}

Value empty_window_aggregate_value(const FunctionValue& fn) {
    if (fn.kind == FunctionValue::Kind::Builtin && fn.name == "count") {
        return Value::integer(0);
    }
    return Value::null();
}

absl::StatusOr<NumericSummary> summarize_numeric_array(const ArrayValue& array,
                                                       const std::string& name) {
    NumericSummary summary;
    for (const auto& item : array.elements) {
        switch (item.type()) {
            case Value::Type::UInt:
                if (summary.kind == NumericKind::Float) {
                    summary.float_sum += static_cast<double>(item.as_uint());
                } else if (summary.kind == NumericKind::Int) {
                    summary.int_sum += static_cast<int64_t>(item.as_uint());
                } else {
                    summary.uint_sum += item.as_uint();
                }
                break;
            case Value::Type::Int:
                if (summary.kind == NumericKind::Float) {
                    summary.float_sum += static_cast<double>(item.as_int());
                } else {
                    if (summary.kind == NumericKind::UInt) {
                        summary.kind = NumericKind::Int;
                        summary.int_sum = static_cast<int64_t>(summary.uint_sum);
                    }
                    summary.int_sum += item.as_int();
                }
                break;
            case Value::Type::Float:
                if (summary.kind == NumericKind::UInt) {
                    summary.float_sum = static_cast<double>(summary.uint_sum);
                } else if (summary.kind == NumericKind::Int) {
                    summary.float_sum = static_cast<double>(summary.int_sum);
                }
                summary.kind = NumericKind::Float;
                summary.float_sum += item.as_float();
                break;
            default:
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " expects an array of numeric values"));
        }
    }
    return summary;
}

Value numeric_sum_value(const NumericSummary& summary) {
    switch (summary.kind) {
        case NumericKind::UInt:
            return Value::uinteger(summary.uint_sum);
        case NumericKind::Int:
            return Value::integer(summary.int_sum);
        case NumericKind::Float:
            return Value::floating(summary.float_sum);
    }
}

absl::StatusOr<Value> aggregate_min_max(const std::vector<Value>& args,
                                        const std::string& name,
                                        bool choose_min) {
    auto array_or = require_array_argument(args, name);
    if (!array_or.ok()) {
        return array_or.status();
    }
    const auto& elements = (*array_or)->elements;
    if (elements.empty()) {
        return absl::InvalidArgumentError(absl::StrCat(name, " expects a non-empty array"));
    }
    const Value* best = &elements[0];
    auto best_number = best->type() == Value::Type::Float ? best->as_float()
                       : best->type() == Value::Type::Int ? static_cast<double>(best->as_int())
                                                          : static_cast<double>(best->as_uint());
    for (size_t i = 1; i < elements.size(); ++i) {
        const auto& item = elements[i];
        if (item.type() != Value::Type::UInt && item.type() != Value::Type::Int &&
            item.type() != Value::Type::Float) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " expects an array of numeric values"));
        }
        auto number = item.type() == Value::Type::Float ? item.as_float()
                      : item.type() == Value::Type::Int ? static_cast<double>(item.as_int())
                                                        : static_cast<double>(item.as_uint());
        if ((choose_min && number < best_number) || (!choose_min && number > best_number)) {
            best = &item;
            best_number = number;
        }
    }
    return *best;
}

absl::StatusOr<Value> builtin_len(const std::vector<Value>& args) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError("len expects exactly one argument");
    }
    switch (args[0].type()) {
        case Value::Type::String:
            return Value::integer(static_cast<int64_t>(args[0].as_string().size()));
        case Value::Type::Array:
            return Value::integer(static_cast<int64_t>(args[0].as_array().elements.size()));
        case Value::Type::Object:
            return Value::integer(static_cast<int64_t>(args[0].as_object().properties.size()));
        default:
            return absl::InvalidArgumentError("len expects string, array, or object");
    }
}

absl::StatusOr<Value> builtin_string(const std::vector<Value>& args) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError("string expects exactly one argument");
    }
    return Value::string(args[0].type() == Value::Type::String ? args[0].as_string()
                                                               : args[0].string());
}

absl::StatusOr<Value> builtin_contains(const std::vector<Value>& args) {
    if (args.size() != 1 || args[0].type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            "contains expects a single object argument with `set` and `value`");
    }
    const auto& object = args[0].as_object();
    const Value* set = object.lookup("set");
    const Value* value = object.lookup("value");
    if (set == nullptr || value == nullptr) {
        return absl::InvalidArgumentError("contains requires `set` and `value`");
    }
    if (set->type() != Value::Type::Array) {
        return absl::InvalidArgumentError("contains `set` must be an array");
    }
    for (const auto& item : set->as_array().elements) {
        if (item == *value) {
            return Value::boolean(true);
        }
    }
    return Value::boolean(false);
}

absl::StatusOr<Value> builtin_sum(const std::vector<Value>& args) {
    auto array_or = require_array_argument(args, "sum");
    if (!array_or.ok()) {
        return array_or.status();
    }
    auto summary_or = summarize_numeric_array(**array_or, "sum");
    if (!summary_or.ok()) {
        return summary_or.status();
    }
    return numeric_sum_value(*summary_or);
}

absl::StatusOr<Value> builtin_mean(const std::vector<Value>& args) {
    auto array_or = require_array_argument(args, "mean");
    if (!array_or.ok()) {
        return array_or.status();
    }
    if ((*array_or)->elements.empty()) {
        return absl::InvalidArgumentError("mean expects a non-empty array");
    }
    auto summary_or = summarize_numeric_array(**array_or, "mean");
    if (!summary_or.ok()) {
        return summary_or.status();
    }
    const auto count = static_cast<double>((*array_or)->elements.size());
    switch (summary_or->kind) {
        case NumericKind::UInt:
            return Value::floating(static_cast<double>(summary_or->uint_sum) / count);
        case NumericKind::Int:
            return Value::floating(static_cast<double>(summary_or->int_sum) / count);
        case NumericKind::Float:
            return Value::floating(summary_or->float_sum / count);
    }
}

absl::StatusOr<Value> builtin_min(const std::vector<Value>& args) {
    return aggregate_min_max(args, "min", true);
}

absl::StatusOr<Value> builtin_max(const std::vector<Value>& args) {
    return aggregate_min_max(args, "max", false);
}

absl::StatusOr<Value> builtin_from(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "from");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto bucket_or = require_object_property(**object_or, "from", "bucket");
    if (!bucket_or.ok()) {
        return bucket_or.status();
    }
    if ((*bucket_or)->type() != Value::Type::String) {
        return absl::InvalidArgumentError("from `bucket` must be a string");
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    if (const Value* rows_value = (*object_or)->lookup("rows"); rows_value != nullptr) {
        auto rows_or = require_table_rows(*rows_value, "from");
        if (!rows_or.ok()) {
            return rows_or.status();
        }
        rows = std::move(*rows_or);
    }
    return Value::table((*bucket_or)->as_string(), std::move(rows));
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

absl::StatusOr<Value> builtin_yield(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "yield");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "yield", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto name_or = optional_string_property(
        **object_or, "yield", "name",
        (*table_or)->result_name.has_value() ? *(*table_or)->result_name : "_result");
    if (!name_or.ok()) {
        return name_or.status();
    }
    return Value::table((*table_or)->bucket, (*table_or)->rows, (*table_or)->range_start,
                        (*table_or)->range_stop, *name_or);
}

absl::StatusOr<Value> builtin_columns(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "columns");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "columns", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    for (const auto& column : visible_columns_in_order(**table_or)) {
        rows.push_back(std::make_shared<ObjectValue>(
            std::vector<std::pair<std::string, Value>>{{"_value", Value::string(column)}}));
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_keys(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "keys");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "keys", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    for (const auto& column : group_columns_in_order(**table_or)) {
        rows.push_back(std::make_shared<ObjectValue>(
            std::vector<std::pair<std::string, Value>>{{"_value", Value::string(column)}}));
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_find_column(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "findColumn");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "findColumn", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "findColumn", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    auto column_or = string_property(**object_or, "findColumn", "column");
    if (!column_or.ok()) {
        return column_or.status();
    }

    auto rows_or = filter_rows_by_function(**table_or, **fn_or, "findColumn");
    if (!rows_or.ok()) {
        return rows_or.status();
    }

    std::vector<Value> values;
    values.reserve(rows_or->size());
    for (const auto& row : *rows_or) {
        const Value* value = row->lookup(*column_or);
        if (value != nullptr && !value->is_null()) {
            values.push_back(*value);
        }
    }
    return Value::array(std::move(values));
}

absl::StatusOr<Value> builtin_find_record(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "findRecord");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "findRecord", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "findRecord", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    auto idx_or = optional_index_property(**object_or, "findRecord", "idx", 0);
    if (!idx_or.ok()) {
        return idx_or.status();
    }

    auto rows_or = filter_rows_by_function(**table_or, **fn_or, "findRecord");
    if (!rows_or.ok()) {
        return rows_or.status();
    }
    if (*idx_or >= rows_or->size()) {
        return Value::null();
    }
    return Value::object((*rows_or)[*idx_or]->properties);
}

absl::StatusOr<Value> builtin_range(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "range");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "range", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto start_or = require_object_property(**object_or, "range", "start");
    if (!start_or.ok()) {
        return start_or.status();
    }
    const auto start = (*start_or)->string();
    const auto stop = optional_literal_property(**object_or, "stop");

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr && row_matches_time_bounds(*row, start, stop)) {
            rows.push_back(row);
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), start, stop);
}

absl::StatusOr<Value> builtin_filter(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "filter");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "filter", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "filter", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    if ((*fn_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError("filter `fn` must be a function");
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        auto keep_or = ExpressionEvaluator::Invoke(**fn_or, {Value::object(row->properties)});
        if (!keep_or.ok()) {
            return keep_or.status();
        }
        if (keep_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("filter `fn` must return a boolean");
        }
        if (keep_or->as_bool()) {
            rows.push_back(row);
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_map(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "map");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "map", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "map", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    if ((*fn_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError("map `fn` must be a function");
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        auto mapped_or = ExpressionEvaluator::Invoke(**fn_or, {Value::object(row->properties)});
        if (!mapped_or.ok()) {
            return mapped_or.status();
        }
        if (mapped_or->type() != Value::Type::Object) {
            return absl::InvalidArgumentError("map `fn` must return an object");
        }
        rows.push_back(std::make_shared<ObjectValue>(mapped_or->as_object()));
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_limit(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "limit");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "limit", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto n_or = integer_property(**object_or, "limit", "n");
    if (!n_or.ok()) {
        return n_or.status();
    }
    if (*n_or < 0) {
        return absl::InvalidArgumentError("limit `n` must be non-negative");
    }
    int64_t offset = 0;
    if (const Value* offset_value = (*object_or)->lookup("offset"); offset_value != nullptr) {
        if (offset_value->type() == Value::Type::Int) {
            offset = offset_value->as_int();
        } else if (offset_value->type() == Value::Type::UInt) {
            offset = static_cast<int64_t>(offset_value->as_uint());
        } else {
            return absl::InvalidArgumentError("limit `offset` must be an int or uint");
        }
        if (offset < 0) {
            return absl::InvalidArgumentError("limit `offset` must be non-negative");
        }
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    const auto begin = static_cast<size_t>(offset);
    const size_t end = std::min((*table_or)->rows.size(), begin + static_cast<size_t>(*n_or));
    if (begin < (*table_or)->rows.size()) {
        rows.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
            if ((*table_or)->rows[i] != nullptr) {
                rows.push_back((*table_or)->rows[i]);
            }
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_tail(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "tail");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "tail", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto n_or = integer_property(**object_or, "tail", "n");
    if (!n_or.ok()) {
        return n_or.status();
    }
    if (*n_or < 0) {
        return absl::InvalidArgumentError("tail `n` must be non-negative");
    }
    int64_t offset = 0;
    if (const Value* offset_value = (*object_or)->lookup("offset"); offset_value != nullptr) {
        if (offset_value->type() == Value::Type::Int) {
            offset = offset_value->as_int();
        } else if (offset_value->type() == Value::Type::UInt) {
            offset = static_cast<int64_t>(offset_value->as_uint());
        } else {
            return absl::InvalidArgumentError("tail `offset` must be an int or uint");
        }
        if (offset < 0) {
            return absl::InvalidArgumentError("tail `offset` must be non-negative");
        }
    }

    const size_t row_count = (*table_or)->rows.size();
    const size_t tail_end =
        offset >= static_cast<int64_t>(row_count) ? 0 : row_count - static_cast<size_t>(offset);
    const size_t tail_begin =
        static_cast<size_t>(*n_or) >= tail_end ? 0 : tail_end - static_cast<size_t>(*n_or);

    std::vector<std::shared_ptr<ObjectValue>> rows;
    if (tail_begin < tail_end) {
        rows.reserve(tail_end - tail_begin);
        for (size_t i = tail_begin; i < tail_end; ++i) {
            if ((*table_or)->rows[i] != nullptr) {
                rows.push_back((*table_or)->rows[i]);
            }
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_keep(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "keep");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "keep", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto columns_or = string_array_property(**object_or, "keep", "columns");
    if (!columns_or.ok()) {
        return columns_or.status();
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr) {
            rows.push_back(clone_row_with_selected_columns(*row, *columns_or, true));
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_drop(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "drop");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "drop", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto columns_or = string_array_property(**object_or, "drop", "columns");
    if (!columns_or.ok()) {
        return columns_or.status();
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr) {
            rows.push_back(clone_row_with_selected_columns(*row, *columns_or, false));
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_rename(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "rename");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "rename", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto columns_or = string_map_property(**object_or, "rename", "columns");
    if (!columns_or.ok()) {
        return columns_or.status();
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        std::vector<std::pair<std::string, Value>> props;
        props.reserve(row->properties.size());
        for (const auto& [key, value] : row->properties) {
            if (auto renamed = mapped_column_name(*columns_or, key); renamed.has_value()) {
                props.emplace_back(*renamed, value);
            } else {
                props.emplace_back(key, value);
            }
        }
        rows.push_back(std::make_shared<ObjectValue>(std::move(props)));
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_duplicate(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "duplicate");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "duplicate", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto column_or = string_property(**object_or, "duplicate", "column");
    if (!column_or.ok()) {
        return column_or.status();
    }
    auto as_or = string_property(**object_or, "duplicate", "as");
    if (!as_or.ok()) {
        return as_or.status();
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* value = row->lookup(*column_or);
        if (value == nullptr) {
            rows.push_back(clone_row(*row));
            continue;
        }
        auto duplicated = object_with_upserted_property(*row, *as_or, *value);
        rows.push_back(std::make_shared<ObjectValue>(duplicated.as_object()));
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_set(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "set");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "set", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto key_or = string_property(**object_or, "set", "key");
    if (!key_or.ok()) {
        return key_or.status();
    }
    auto value_or = require_object_property(**object_or, "set", "value");
    if (!value_or.ok()) {
        return value_or.status();
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr) {
            auto updated = object_with_upserted_property(*row, *key_or, **value_or);
            rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_reduce(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "reduce");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "reduce", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "reduce", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    auto identity_or = require_object_property(**object_or, "reduce", "identity");
    if (!identity_or.ok()) {
        return identity_or.status();
    }
    if ((*fn_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError("reduce `fn` must be a function");
    }
    if ((*identity_or)->type() != Value::Type::Object) {
        return absl::InvalidArgumentError("reduce `identity` must be an object");
    }

    Value accumulator = **identity_or;
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        auto next_or =
            ExpressionEvaluator::Invoke(**fn_or, {Value::object(row->properties), accumulator});
        if (!next_or.ok()) {
            return next_or.status();
        }
        if (next_or->type() != Value::Type::Object) {
            return absl::InvalidArgumentError("reduce `fn` must return an object");
        }
        accumulator = *next_or;
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.push_back(std::make_shared<ObjectValue>(accumulator.as_object()));
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_sort(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "sort");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "sort", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto columns_or = optional_string_array_property(**object_or, "sort", "columns", {"_value"});
    if (!columns_or.ok()) {
        return columns_or.status();
    }
    auto desc_or = optional_bool_property(**object_or, "sort", "desc", false);
    if (!desc_or.ok()) {
        return desc_or.status();
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr) {
            rows.push_back(row);
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [&](const auto& lhs, const auto& rhs) {
        for (const auto& column : *columns_or) {
            const int cmp = compare_values(lhs->lookup(column), rhs->lookup(column));
            if (cmp != 0) {
                return *desc_or ? cmp > 0 : cmp < 0;
            }
        }
        return false;
    });
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_group(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "group");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "group", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto columns_or = optional_string_array_property(**object_or, "group", "columns", {});
    if (!columns_or.ok()) {
        return columns_or.status();
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr) {
            rows.push_back(clone_row_with_group(*row, *columns_or));
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_pivot(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "pivot");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "pivot", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto row_key_or = string_array_property(**object_or, "pivot", "rowKey");
    if (!row_key_or.ok()) {
        return row_key_or.status();
    }
    auto column_key_or = string_array_property(**object_or, "pivot", "columnKey");
    if (!column_key_or.ok()) {
        return column_key_or.status();
    }
    auto value_column_or = string_property(**object_or, "pivot", "valueColumn");
    if (!value_column_or.ok()) {
        return value_column_or.status();
    }
    if (column_key_or->empty()) {
        return absl::InvalidArgumentError("pivot `columnKey` must not be empty");
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    std::unordered_map<std::string, size_t> row_indexes;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const std::string identity = row_identity_key(*row, *row_key_or);
        size_t row_index = 0;
        if (const auto existing = row_indexes.find(identity); existing != row_indexes.end()) {
            row_index = existing->second;
        } else {
            std::vector<std::pair<std::string, Value>> props;
            std::unordered_set<std::string> included;
            for (const auto& column : *row_key_or) {
                if (const Value* value = row->lookup(column); value != nullptr) {
                    props.emplace_back(column, *value);
                    included.insert(column);
                }
            }
            for (const auto& [key, value] : row->properties) {
                if (key == *value_column_or || contains_string(*column_key_or, key) ||
                    included.count(key) != 0) {
                    continue;
                }
                props.emplace_back(key, value);
                included.insert(key);
            }
            row_index = rows.size();
            rows.push_back(std::make_shared<ObjectValue>(std::move(props)));
            row_indexes.emplace(identity, row_index);
        }

        const Value* value = row->lookup(*value_column_or);
        if (value == nullptr) {
            continue;
        }
        const std::string pivoted_name = pivot_column_name(*row, *column_key_or);
        auto updated = object_with_upserted_property(*rows[row_index], pivoted_name, *value);
        rows[row_index] = std::make_shared<ObjectValue>(updated.as_object());
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_fill(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "fill");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "fill", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto column_or = optional_string_property(**object_or, "fill", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }
    auto use_previous_or = optional_bool_property(**object_or, "fill", "usePrevious", false);
    if (!use_previous_or.ok()) {
        return use_previous_or.status();
    }
    const Value* explicit_value = (*object_or)->lookup("value");
    if (!*use_previous_or && explicit_value == nullptr) {
        return absl::InvalidArgumentError("fill requires either `usePrevious: true` or a `value`");
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    std::unordered_map<std::string, Value> previous_by_group;
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* current = row->lookup(*column_or);
        const bool needs_fill = current == nullptr || current->is_null();
        auto next_row = clone_row(*row);
        const std::string group_key = group_key_for_row(*row);

        if (needs_fill) {
            std::optional<Value> replacement;
            if (*use_previous_or) {
                if (const auto previous = previous_by_group.find(group_key);
                    previous != previous_by_group.end()) {
                    replacement = previous->second;
                }
            } else if (explicit_value != nullptr) {
                replacement = *explicit_value;
            }
            if (replacement.has_value()) {
                auto updated = object_with_upserted_property(*next_row, *column_or, *replacement);
                next_row = std::make_shared<ObjectValue>(updated.as_object());
                previous_by_group[group_key] = *replacement;
            }
        } else {
            previous_by_group[group_key] = *current;
        }
        rows.push_back(next_row);
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_elapsed(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "elapsed");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "elapsed", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto time_column_or = optional_string_property(**object_or, "elapsed", "timeColumn", "_time");
    if (!time_column_or.ok()) {
        return time_column_or.status();
    }
    auto column_name_or = optional_string_property(**object_or, "elapsed", "columnName", "elapsed");
    if (!column_name_or.ok()) {
        return column_name_or.status();
    }

    int64_t unit_seconds = 1;
    if (const Value* unit_value = (*object_or)->lookup("unit"); unit_value != nullptr) {
        auto unit_or = parse_window_duration(*unit_value, "elapsed", "unit");
        if (!unit_or.ok()) {
            return unit_or.status();
        }
        if (unit_or->kind != WindowDuration::Kind::FixedSeconds) {
            return absl::InvalidArgumentError("elapsed `unit` does not support calendar durations");
        }
        unit_seconds = unit_or->seconds;
    }

    std::unordered_map<std::string, int64_t> previous_time_by_group;
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* time_value = row->lookup(*time_column_or);
        if (time_value == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("elapsed requires `", *time_column_or, "` on every row"));
        }

        std::string literal;
        if (time_value->type() == Value::Type::Time) {
            literal = time_value->as_time().literal;
        } else if (time_value->type() == Value::Type::String) {
            literal = time_value->as_string();
        } else {
            return absl::InvalidArgumentError(
                absl::StrCat("elapsed `", *time_column_or, "` must be a time or string"));
        }
        auto seconds_or = parse_rfc3339_seconds(literal);
        if (!seconds_or.has_value()) {
            return absl::InvalidArgumentError(
                absl::StrCat("elapsed could not parse RFC3339 time: ", literal));
        }

        const std::string group_key = group_key_for_row(*row);
        if (const auto previous = previous_time_by_group.find(group_key);
            previous != previous_time_by_group.end()) {
            auto updated = object_with_upserted_property(
                *row, *column_name_or,
                Value::integer((*seconds_or - previous->second) / unit_seconds));
            rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        }
        previous_time_by_group[group_key] = *seconds_or;
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_difference(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "difference");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "difference", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto column_or = optional_string_property(**object_or, "difference", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }
    auto non_negative_or = optional_bool_property(**object_or, "difference", "nonNegative", false);
    if (!non_negative_or.ok()) {
        return non_negative_or.status();
    }
    auto keep_first_or = optional_bool_property(**object_or, "difference", "keepFirst", false);
    if (!keep_first_or.ok()) {
        return keep_first_or.status();
    }

    std::unordered_map<std::string, Value> previous_by_group;
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* current = row->lookup(*column_or);
        if (current == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("difference requires `", *column_or, "` on every row"));
        }
        if (!is_numeric_value(*current)) {
            return absl::InvalidArgumentError(
                absl::StrCat("difference `", *column_or, "` must be numeric"));
        }

        const std::string group_key = group_key_for_row(*row);
        if (const auto previous = previous_by_group.find(group_key);
            previous != previous_by_group.end()) {
            const double delta = numeric_value(*current) - numeric_value(previous->second);
            Value difference = Value::null();
            if (!*non_negative_or || delta >= 0.0) {
                if (current->type() == Value::Type::Float ||
                    previous->second.type() == Value::Type::Float) {
                    difference = Value::floating(delta);
                } else {
                    difference = Value::integer(static_cast<int64_t>(delta));
                }
            }
            auto updated = object_with_upserted_property(*row, *column_or, std::move(difference));
            rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        } else if (*keep_first_or) {
            auto updated = object_with_upserted_property(*row, *column_or, Value::null());
            rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        }
        previous_by_group[group_key] = *current;
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_derivative(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "derivative");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "derivative", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto column_or = optional_string_property(**object_or, "derivative", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }
    auto time_column_or =
        optional_string_property(**object_or, "derivative", "timeColumn", "_time");
    if (!time_column_or.ok()) {
        return time_column_or.status();
    }
    auto non_negative_or = optional_bool_property(**object_or, "derivative", "nonNegative", false);
    if (!non_negative_or.ok()) {
        return non_negative_or.status();
    }
    auto initial_zero_or = optional_bool_property(**object_or, "derivative", "initialZero", false);
    if (!initial_zero_or.ok()) {
        return initial_zero_or.status();
    }

    int64_t unit_seconds = 1;
    if (const Value* unit_value = (*object_or)->lookup("unit"); unit_value != nullptr) {
        auto unit_or = parse_window_duration(*unit_value, "derivative", "unit");
        if (!unit_or.ok()) {
            return unit_or.status();
        }
        if (unit_or->kind != WindowDuration::Kind::FixedSeconds) {
            return absl::InvalidArgumentError(
                "derivative `unit` does not support calendar durations");
        }
        unit_seconds = unit_or->seconds;
    }

    std::unordered_map<std::string, Value> previous_value_by_group;
    std::unordered_map<std::string, int64_t> previous_time_by_group;
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* current = row->lookup(*column_or);
        if (current == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative requires `", *column_or, "` on every row"));
        }
        if (!is_numeric_value(*current)) {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative `", *column_or, "` must be numeric"));
        }

        const Value* time_value = row->lookup(*time_column_or);
        if (time_value == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative requires `", *time_column_or, "` on every row"));
        }

        std::string literal;
        if (time_value->type() == Value::Type::Time) {
            literal = time_value->as_time().literal;
        } else if (time_value->type() == Value::Type::String) {
            literal = time_value->as_string();
        } else {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative `", *time_column_or, "` must be a time or string"));
        }
        auto seconds_or = parse_rfc3339_seconds(literal);
        if (!seconds_or.has_value()) {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative could not parse RFC3339 time: ", literal));
        }

        const std::string group_key = group_key_for_row(*row);
        auto previous_value = previous_value_by_group.find(group_key);
        auto previous_time = previous_time_by_group.find(group_key);
        if (previous_value != previous_value_by_group.end() &&
            previous_time != previous_time_by_group.end()) {
            const int64_t delta_seconds = *seconds_or - previous_time->second;
            if (delta_seconds == 0) {
                return absl::InvalidArgumentError(
                    "derivative requires strictly increasing time within each group");
            }
            const double raw_delta =
                numeric_value(*current) - numeric_value(previous_value->second);
            Value rate = Value::null();
            if (!*non_negative_or || raw_delta >= 0.0) {
                rate = Value::floating(raw_delta * static_cast<double>(unit_seconds) /
                                       static_cast<double>(delta_seconds));
            } else if (*initial_zero_or) {
                rate = Value::floating(numeric_value(*current) * static_cast<double>(unit_seconds) /
                                       static_cast<double>(delta_seconds));
            }
            auto updated = object_with_upserted_property(*row, *column_or, std::move(rate));
            rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        }
        previous_value_by_group[group_key] = *current;
        previous_time_by_group[group_key] = *seconds_or;
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_distinct(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "distinct");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "distinct", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto column_or = optional_string_property(**object_or, "distinct", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }

    std::unordered_set<std::string> seen;
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* value = row->lookup(*column_or);
        const std::string key = absl::StrCat(group_key_for_row(*row), "\n",
                                             value == nullptr ? "<missing>" : value->string());
        if (!seen.insert(key).second) {
            continue;
        }
        rows.push_back(row);
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_count(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "count");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "count", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    std::string column = "_value";
    if (const Value* column_value = (*object_or)->lookup("column"); column_value != nullptr) {
        if (column_value->type() != Value::Type::String) {
            return absl::InvalidArgumentError("count `column` must be a string");
        }
        column = column_value->as_string();
    }

    int64_t count = 0;
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr && row->lookup(column) != nullptr) {
            ++count;
        }
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.push_back(std::make_shared<ObjectValue>(
        std::vector<std::pair<std::string, Value>>{{column, Value::integer(count)}}));
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> table_single_row_builtin(const std::vector<Value>& args,
                                               const std::string& name,
                                               bool use_last) {
    auto object_or = require_object_argument(args, name);
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, name, "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    if (!(*table_or)->rows.empty()) {
        const auto& source = use_last ? (*table_or)->rows.back() : (*table_or)->rows.front();
        if (source != nullptr) {
            rows.push_back(source);
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_first(const std::vector<Value>& args) {
    return table_single_row_builtin(args, "first", false);
}

absl::StatusOr<Value> builtin_last(const std::vector<Value>& args) {
    return table_single_row_builtin(args, "last", true);
}

absl::StatusOr<Value> builtin_union(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "union");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto tables_or = require_table_array_property(**object_or, "union", "tables");
    if (!tables_or.ok()) {
        return tables_or.status();
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    std::string bucket = "union";
    std::optional<std::string> range_start;
    std::optional<std::string> range_stop;
    for (const auto* table : *tables_or) {
        if (table == nullptr) {
            continue;
        }
        if (bucket == "union" && !table->bucket.empty()) {
            bucket = table->bucket;
        }
        if (!range_start.has_value()) {
            range_start = table->range_start;
        }
        if (!range_stop.has_value()) {
            range_stop = table->range_stop;
        }
        rows.reserve(rows.size() + table->rows.size());
        for (const auto& row : table->rows) {
            if (row != nullptr) {
                rows.push_back(row);
            }
        }
    }
    return Value::table(bucket, std::move(rows), range_start, range_stop);
}

absl::StatusOr<Value> builtin_join(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "join");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto tables_or = require_named_table_object_property(**object_or, "join", "tables");
    if (!tables_or.ok()) {
        return tables_or.status();
    }
    if (tables_or->size() != 2) {
        return absl::InvalidArgumentError("join currently expects exactly two input tables");
    }
    auto on_or = string_array_property(**object_or, "join", "on");
    if (!on_or.ok()) {
        return on_or.status();
    }

    const auto& left_name = (*tables_or)[0].first;
    const auto* left_table = (*tables_or)[0].second;
    const auto& right_name = (*tables_or)[1].first;
    const auto* right_table = (*tables_or)[1].second;

    std::unordered_map<std::string, std::vector<const ObjectValue*>> right_rows_by_key;
    right_rows_by_key.reserve(right_table->rows.size());
    for (const auto& right_row : right_table->rows) {
        if (right_row == nullptr) {
            continue;
        }
        right_rows_by_key[row_identity_key(*right_row, *on_or)].push_back(right_row.get());
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve(std::min(left_table->rows.size(), right_table->rows.size()));
    for (const auto& left_row : left_table->rows) {
        if (left_row == nullptr) {
            continue;
        }
        const auto matches = right_rows_by_key.find(row_identity_key(*left_row, *on_or));
        if (matches == right_rows_by_key.end()) {
            continue;
        }
        for (const auto* right_row : matches->second) {
            rows.push_back(join_rows(left_name, *left_row, right_name, *right_row, *on_or));
        }
    }
    return Value::table(
        left_table->bucket.empty() ? right_table->bucket : left_table->bucket, std::move(rows),
        left_table->range_start.has_value() ? left_table->range_start : right_table->range_start,
        left_table->range_stop.has_value() ? left_table->range_stop : right_table->range_stop);
}

absl::StatusOr<Value> builtin_aggregate_window(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "aggregateWindow");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "aggregateWindow", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto every_value_or = require_object_property(**object_or, "aggregateWindow", "every");
    if (!every_value_or.ok()) {
        return every_value_or.status();
    }
    auto every_or = parse_window_duration(**every_value_or, "aggregateWindow", "every");
    if (!every_or.ok()) {
        return every_or.status();
    }
    auto location_or = optional_window_location_property(**object_or, "aggregateWindow");
    if (!location_or.ok()) {
        return location_or.status();
    }
    WindowDuration offset{
        .kind = WindowDuration::Kind::FixedSeconds,
        .seconds = 0,
        .months = 0,
    };
    if (const Value* offset_value = (*object_or)->lookup("offset"); offset_value != nullptr) {
        auto offset_or =
            parse_window_duration(*offset_value, "aggregateWindow", "offset", true, true);
        if (!offset_or.ok()) {
            return offset_or.status();
        }
        offset = *offset_or;
    }
    if (every_or->kind == WindowDuration::Kind::FixedSeconds &&
        offset.kind != WindowDuration::Kind::FixedSeconds) {
        return absl::InvalidArgumentError(
            "aggregateWindow fixed-duration windows do not support calendar `offset`");
    }
    WindowDuration period = *every_or;
    if (const Value* period_value = (*object_or)->lookup("period"); period_value != nullptr) {
        auto period_or =
            parse_window_duration(*period_value, "aggregateWindow", "period", true, false);
        if (!period_or.ok()) {
            return period_or.status();
        }
        period = *period_or;
    }
    if (every_or->kind == WindowDuration::Kind::FixedSeconds &&
        period.kind != WindowDuration::Kind::FixedSeconds) {
        return absl::InvalidArgumentError(
            "aggregateWindow fixed-duration windows do not support calendar `period`");
    }
    auto fn_or = require_object_property(**object_or, "aggregateWindow", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    if ((*fn_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError("aggregateWindow `fn` must be a function");
    }
    auto column_or = optional_string_property(**object_or, "aggregateWindow", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }
    auto time_column_or =
        optional_string_property(**object_or, "aggregateWindow", "timeColumn", "_time");
    if (!time_column_or.ok()) {
        return time_column_or.status();
    }
    auto time_src_or = optional_string_property(**object_or, "aggregateWindow", "timeSrc", "_stop");
    if (!time_src_or.ok()) {
        return time_src_or.status();
    }
    auto time_dst_or = optional_string_property(**object_or, "aggregateWindow", "timeDst", "_time");
    if (!time_dst_or.ok()) {
        return time_dst_or.status();
    }
    auto create_empty_or =
        optional_bool_property(**object_or, "aggregateWindow", "createEmpty", true);
    if (!create_empty_or.ok()) {
        return create_empty_or.status();
    }
    std::optional<int64_t> table_range_start_seconds;
    std::optional<int64_t> table_range_stop_seconds;
    if ((*table_or)->range_start.has_value()) {
        table_range_start_seconds = parse_rfc3339_seconds(*(*table_or)->range_start);
    }
    if ((*table_or)->range_stop.has_value()) {
        table_range_stop_seconds = parse_rfc3339_seconds(*(*table_or)->range_stop);
    }
    std::vector<AggregateWindowBucket> buckets;
    std::vector<AggregateWindowGroupSpan> group_spans;
    std::unordered_map<std::string, size_t> bucket_indexes;
    std::unordered_map<std::string, size_t> group_span_indexes;
    bucket_indexes.reserve((*table_or)->rows.size());
    group_span_indexes.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* aggregate_value = row->lookup(*column_or);
        if (aggregate_value == nullptr) {
            continue;
        }

        std::optional<int64_t> row_seconds;
        if (const Value* time_value = row->lookup(*time_column_or); time_value != nullptr) {
            std::optional<std::string> literal;
            if (time_value->type() == Value::Type::Time) {
                literal = time_value->as_time().literal;
            } else if (time_value->type() == Value::Type::String) {
                literal = time_value->as_string();
            }
            if (literal.has_value()) {
                if (auto seconds = parse_rfc3339_seconds(*literal); seconds.has_value()) {
                    row_seconds = *seconds;
                }
            }
        }

        const auto group_key = group_key_for_row(*row);
        if (*create_empty_or && row_seconds.has_value()) {
            const auto span_it = group_span_indexes.find(group_key);
            if (span_it == group_span_indexes.end()) {
                group_span_indexes.emplace(group_key, group_spans.size());
                const int64_t min_time = table_range_start_seconds.has_value()
                                             ? *table_range_start_seconds
                                             : *row_seconds;
                const int64_t max_time_exclusive = table_range_stop_seconds.has_value()
                                                       ? *table_range_stop_seconds
                                                       : *row_seconds + 1;
                group_spans.push_back(AggregateWindowGroupSpan{
                    .group_key = group_key,
                    .template_row = aggregate_window_base_row(*row, *column_or, *time_dst_or),
                    .min_time_seconds = min_time,
                    .max_time_exclusive_seconds = max_time_exclusive});
            } else {
                auto* span = &group_spans[span_it->second];
                if (!table_range_start_seconds.has_value() &&
                    *row_seconds < span->min_time_seconds) {
                    span->min_time_seconds = *row_seconds;
                }
                if (!table_range_stop_seconds.has_value() &&
                    *row_seconds + 1 > span->max_time_exclusive_seconds) {
                    span->max_time_exclusive_seconds = *row_seconds + 1;
                }
            }
        }
        if (!row_seconds.has_value()) {
            const std::string bucket_key = window_bucket_key(std::nullopt, group_key);
            const auto bucket_it = bucket_indexes.find(bucket_key);
            if (bucket_it == bucket_indexes.end()) {
                bucket_indexes.emplace(bucket_key, buckets.size());
                buckets.push_back(AggregateWindowBucket{
                    .start_seconds = std::nullopt,
                    .group_key = group_key,
                    .first_row = aggregate_window_base_row(*row, *column_or, *time_dst_or),
                    .values = {},
                });
                buckets.back().values.push_back(*aggregate_value);
            } else {
                buckets[bucket_it->second].values.push_back(*aggregate_value);
            }
            continue;
        }
        auto primary_start_or =
            aggregate_window_start_for_time(*row_seconds, *every_or, offset, *location_or);
        if (!primary_start_or.has_value()) {
            continue;
        }
        std::vector<int64_t> candidate_starts;
        if (!window_duration_is_negative(period)) {
            for (std::optional<int64_t> current = primary_start_or; current.has_value();) {
                auto bounds_or = aggregate_window_bounds_for_start(*current, period, *location_or);
                if (!bounds_or.has_value()) {
                    break;
                }
                const bool within_table_range =
                    !table_range_start_seconds.has_value() ||
                    !table_range_stop_seconds.has_value() ||
                    aggregate_window_is_within_range(*bounds_or, *table_range_start_seconds,
                                                     *table_range_stop_seconds);
                if (within_table_range &&
                    aggregate_window_contains_time(*row_seconds, *bounds_or)) {
                    candidate_starts.push_back(*current);
                }
                if (bounds_or->upper_seconds <= *row_seconds) {
                    break;
                }
                auto previous_or = add_window_duration_to_time(
                    *current, negate_window_duration(*every_or), *location_or);
                if (!previous_or.has_value() || *previous_or >= *current) {
                    break;
                }
                current = previous_or;
            }
        } else {
            auto current = add_window_duration_to_time(*primary_start_or, *every_or, *location_or);
            while (current.has_value()) {
                auto bounds_or = aggregate_window_bounds_for_start(*current, period, *location_or);
                if (!bounds_or.has_value()) {
                    break;
                }
                if (bounds_or->lower_seconds >= *row_seconds + 1) {
                    break;
                }
                const bool within_table_range =
                    !table_range_start_seconds.has_value() ||
                    !table_range_stop_seconds.has_value() ||
                    aggregate_window_is_within_range(*bounds_or, *table_range_start_seconds,
                                                     *table_range_stop_seconds);
                if (within_table_range &&
                    aggregate_window_contains_time(*row_seconds, *bounds_or)) {
                    candidate_starts.push_back(*current);
                }
                auto next_or = add_window_duration_to_time(*current, *every_or, *location_or);
                if (!next_or.has_value() || *next_or <= *current) {
                    break;
                }
                current = next_or;
            }
        }
        for (const auto candidate_start : candidate_starts) {
            const std::string bucket_key = window_bucket_key(candidate_start, group_key);
            const auto bucket_it = bucket_indexes.find(bucket_key);
            if (bucket_it == bucket_indexes.end()) {
                bucket_indexes.emplace(bucket_key, buckets.size());
                buckets.push_back(AggregateWindowBucket{
                    .start_seconds = candidate_start,
                    .group_key = group_key,
                    .first_row = aggregate_window_base_row(*row, *column_or, *time_dst_or),
                    .values = {},
                });
                buckets.back().values.push_back(*aggregate_value);
                continue;
            }
            auto* bucket = &buckets[bucket_it->second];
            bucket->values.push_back(*aggregate_value);
        }
    }

    if (*create_empty_or) {
        for (const auto& span : group_spans) {
            if (span.template_row == nullptr ||
                span.max_time_exclusive_seconds <= span.min_time_seconds) {
                continue;
            }
            auto first_window_start_or = aggregate_window_start_for_time(
                span.min_time_seconds, *every_or, offset, *location_or);
            if (!first_window_start_or.has_value()) {
                continue;
            }
            if (!window_duration_is_negative(period)) {
                while (true) {
                    auto previous_or = add_window_duration_to_time(
                        *first_window_start_or, negate_window_duration(*every_or), *location_or);
                    if (!previous_or.has_value() || *previous_or >= *first_window_start_or) {
                        break;
                    }
                    auto previous_bounds_or =
                        aggregate_window_bounds_for_start(*previous_or, period, *location_or);
                    if (!previous_bounds_or.has_value() ||
                        previous_bounds_or->upper_seconds <= span.min_time_seconds) {
                        break;
                    }
                    first_window_start_or = previous_or;
                }
            } else {
                auto next_or =
                    add_window_duration_to_time(*first_window_start_or, *every_or, *location_or);
                if (!next_or.has_value() || *next_or <= *first_window_start_or) {
                    continue;
                }
                first_window_start_or = next_or;
            }
            for (auto window_start_or = first_window_start_or; window_start_or.has_value();) {
                auto bounds_or =
                    aggregate_window_bounds_for_start(*window_start_or, period, *location_or);
                if (!bounds_or.has_value()) {
                    break;
                }
                if (bounds_or->lower_seconds >= span.max_time_exclusive_seconds) {
                    break;
                }
                if (aggregate_window_intersects_range(*bounds_or, span.min_time_seconds,
                                                      span.max_time_exclusive_seconds) &&
                    (!table_range_start_seconds.has_value() ||
                     !table_range_stop_seconds.has_value() ||
                     aggregate_window_is_within_range(*bounds_or, *table_range_start_seconds,
                                                      *table_range_stop_seconds))) {
                    const std::string bucket_key =
                        window_bucket_key(*window_start_or, span.group_key);
                    if (bucket_indexes.find(bucket_key) == bucket_indexes.end()) {
                        bucket_indexes.emplace(bucket_key, buckets.size());
                        buckets.push_back(AggregateWindowBucket{
                            .start_seconds = *window_start_or,
                            .group_key = span.group_key,
                            .first_row = clone_row(*span.template_row),
                            .values = {},
                        });
                    }
                }
                auto next_or =
                    add_window_duration_to_time(*window_start_or, *every_or, *location_or);
                if (!next_or.has_value() || *next_or <= *window_start_or) {
                    break;
                }
                window_start_or = next_or;
            }
        }
    }

    std::stable_sort(buckets.begin(), buckets.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.start_seconds != rhs.start_seconds) {
            if (!lhs.start_seconds.has_value()) {
                return false;
            }
            if (!rhs.start_seconds.has_value()) {
                return true;
            }
            return *lhs.start_seconds < *rhs.start_seconds;
        }
        return lhs.group_key < rhs.group_key;
    });

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve(buckets.size());
    for (const auto& bucket : buckets) {
        if (bucket.first_row == nullptr) {
            continue;
        }
        Value aggregate_value = Value::null();
        if (bucket.values.empty()) {
            if (!*create_empty_or || aggregate_window_fn_drops_empty((*fn_or)->as_function())) {
                continue;
            }
            aggregate_value = empty_window_aggregate_value((*fn_or)->as_function());
        } else {
            auto aggregate_or = invoke_window_aggregate((*fn_or)->as_function(), bucket.values);
            if (!aggregate_or.ok()) {
                return aggregate_or.status();
            }
            aggregate_value = *aggregate_or;
        }

        Value row_value =
            object_with_upserted_property(*bucket.first_row, *column_or, aggregate_value);
        if (bucket.start_seconds.has_value()) {
            auto bounds_or =
                aggregate_window_bounds_for_start(*bucket.start_seconds, period, *location_or);
            if (!bounds_or.has_value()) {
                return absl::InternalError("aggregateWindow failed to compute window stop");
            }
            const int64_t window_stop = bounds_or->stop_seconds;
            row_value = object_with_upserted_property(
                row_value.as_object(), "_start",
                Value::time(format_rfc3339_seconds(*bucket.start_seconds)));
            row_value = object_with_upserted_property(
                row_value.as_object(), "_stop", Value::time(format_rfc3339_seconds(window_stop)));
            auto time_src_seconds = aggregate_window_time_src_seconds(
                bucket.first_row, *bucket.start_seconds, window_stop, *time_src_or);
            if (time_src_seconds.has_value()) {
                row_value = object_with_upserted_property(
                    row_value.as_object(), *time_dst_or,
                    Value::time(format_rfc3339_seconds(*time_src_seconds)));
            }
        }
        rows.push_back(std::make_shared<ObjectValue>(row_value.as_object()));
    }
    return Value::table((*table_or)->bucket, std::move(rows), (*table_or)->range_start,
                        (*table_or)->range_stop);
}

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

void install_builtin(Environment& env,
                     const std::string& name,
                     FunctionValue::BuiltinCallback fn,
                     std::string pipe_param_name = {}) {
    env.define(name, make_builtin_value(name, std::move(fn), std::move(pipe_param_name)));
}

bool install_known_builtin(Environment& env, const std::string& name) {
    if (name == "len") {
        install_builtin(env, "len", builtin_len);
        return true;
    }
    if (name == "string") {
        install_builtin(env, "string", builtin_string);
        return true;
    }
    if (name == "contains") {
        install_builtin(env, "contains", builtin_contains, "set");
        return true;
    }
    if (name == "sum") {
        install_builtin(env, "sum", builtin_sum, "values");
        return true;
    }
    if (name == "mean") {
        install_builtin(env, "mean", builtin_mean, "values");
        return true;
    }
    if (name == "min") {
        install_builtin(env, "min", builtin_min, "values");
        return true;
    }
    if (name == "max") {
        install_builtin(env, "max", builtin_max, "values");
        return true;
    }
    if (name == "from") {
        install_builtin(env, "from", builtin_from);
        return true;
    }
    if (name == "columns") {
        install_builtin(env, "columns", builtin_columns, "tables");
        return true;
    }
    if (name == "keys") {
        install_builtin(env, "keys", builtin_keys, "tables");
        return true;
    }
    if (name == "findColumn") {
        install_builtin(env, "findColumn", builtin_find_column, "tables");
        return true;
    }
    if (name == "findRecord") {
        install_builtin(env, "findRecord", builtin_find_record, "tables");
        return true;
    }
    if (name == "range") {
        install_builtin(env, "range", builtin_range, "tables");
        return true;
    }
    if (name == "filter") {
        install_builtin(env, "filter", builtin_filter, "tables");
        return true;
    }
    if (name == "map") {
        install_builtin(env, "map", builtin_map, "tables");
        return true;
    }
    if (name == "limit") {
        install_builtin(env, "limit", builtin_limit, "tables");
        return true;
    }
    if (name == "tail") {
        install_builtin(env, "tail", builtin_tail, "tables");
        return true;
    }
    if (name == "keep") {
        install_builtin(env, "keep", builtin_keep, "tables");
        return true;
    }
    if (name == "drop") {
        install_builtin(env, "drop", builtin_drop, "tables");
        return true;
    }
    if (name == "rename") {
        install_builtin(env, "rename", builtin_rename, "tables");
        return true;
    }
    if (name == "duplicate") {
        install_builtin(env, "duplicate", builtin_duplicate, "tables");
        return true;
    }
    if (name == "set") {
        install_builtin(env, "set", builtin_set, "tables");
        return true;
    }
    if (name == "reduce") {
        install_builtin(env, "reduce", builtin_reduce, "tables");
        return true;
    }
    if (name == "sort") {
        install_builtin(env, "sort", builtin_sort, "tables");
        return true;
    }
    if (name == "group") {
        install_builtin(env, "group", builtin_group, "tables");
        return true;
    }
    if (name == "pivot") {
        install_builtin(env, "pivot", builtin_pivot, "tables");
        return true;
    }
    if (name == "fill") {
        install_builtin(env, "fill", builtin_fill, "tables");
        return true;
    }
    if (name == "elapsed") {
        install_builtin(env, "elapsed", builtin_elapsed, "tables");
        return true;
    }
    if (name == "difference") {
        install_builtin(env, "difference", builtin_difference, "tables");
        return true;
    }
    if (name == "derivative") {
        install_builtin(env, "derivative", builtin_derivative, "tables");
        return true;
    }
    if (name == "distinct") {
        install_builtin(env, "distinct", builtin_distinct, "tables");
        return true;
    }
    if (name == "count") {
        install_builtin(env, "count", builtin_count, "tables");
        return true;
    }
    if (name == "first") {
        install_builtin(env, "first", builtin_first, "tables");
        return true;
    }
    if (name == "last") {
        install_builtin(env, "last", builtin_last, "tables");
        return true;
    }
    if (name == "union") {
        install_builtin(env, "union", builtin_union);
        return true;
    }
    if (name == "join") {
        install_builtin(env, "join", builtin_join);
        return true;
    }
    if (name == "aggregateWindow") {
        install_builtin(env, "aggregateWindow", builtin_aggregate_window, "tables");
        return true;
    }
    if (name == "yield") {
        install_builtin(env, "yield", builtin_yield, "tables");
        return true;
    }
    return false;
}

} // namespace

void BuiltinRegistry::Install(Environment& env) {
    install_known_builtin(env, "len");
    install_known_builtin(env, "string");
    install_known_builtin(env, "contains");
    install_known_builtin(env, "sum");
    install_known_builtin(env, "mean");
    install_known_builtin(env, "min");
    install_known_builtin(env, "max");
    install_known_builtin(env, "from");
    install_known_builtin(env, "columns");
    install_known_builtin(env, "keys");
    install_known_builtin(env, "findColumn");
    install_known_builtin(env, "findRecord");
    install_known_builtin(env, "range");
    install_known_builtin(env, "filter");
    install_known_builtin(env, "map");
    install_known_builtin(env, "limit");
    install_known_builtin(env, "tail");
    install_known_builtin(env, "keep");
    install_known_builtin(env, "drop");
    install_known_builtin(env, "rename");
    install_known_builtin(env, "duplicate");
    install_known_builtin(env, "set");
    install_known_builtin(env, "reduce");
    install_known_builtin(env, "sort");
    install_known_builtin(env, "group");
    install_known_builtin(env, "pivot");
    install_known_builtin(env, "fill");
    install_known_builtin(env, "elapsed");
    install_known_builtin(env, "difference");
    install_known_builtin(env, "derivative");
    install_known_builtin(env, "distinct");
    install_known_builtin(env, "count");
    install_known_builtin(env, "first");
    install_known_builtin(env, "last");
    install_known_builtin(env, "union");
    install_known_builtin(env, "join");
    install_known_builtin(env, "aggregateWindow");
    install_known_builtin(env, "yield");
}

absl::Status BuiltinRegistry::Ensure(Environment& env, const std::string& name) {
    auto current = env.lookup(name);
    if (current.ok()) {
        if (current->type() != Value::Type::Function) {
            return absl::InvalidArgumentError(
                absl::StrCat("builtin name conflicts with non-function binding: ", name));
        }
        return absl::OkStatus();
    }
    if (install_known_builtin(env, name)) {
        return absl::OkStatus();
    }
    install_builtin(env, name, [name](const std::vector<Value>&) -> absl::StatusOr<Value> {
        return absl::UnimplementedError(
            absl::StrCat("builtin `", name, "` is declared but not implemented"));
    });
    return absl::OkStatus();
}

absl::StatusOr<Value> BuiltinRegistry::ImportPackage(const std::string& path) {
    if (path == "csv") {
        return Value::object({
            {"path", Value::string("csv")},
            {"from", make_builtin_value("csv.from", builtin_csv_from)},
        });
    }
    return Value::object({{"path", Value::string(path)}});
}

} // namespace pl
