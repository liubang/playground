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
// Created: 2026/04/25 10:40

#pragma once

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "cpp/pl/flux/runtime_env.h"
#include "cpp/pl/flux/runtime_eval.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pl {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

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

absl::StatusOr<std::vector<double>> quantile_values_property(const ObjectValue& object,
                                                             const std::string& name,
                                                             const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    std::vector<double> quantiles;
    const Value& value = **value_or;
    auto append_quantile = [&](const Value& item) -> absl::Status {
        switch (item.type()) {
            case Value::Type::Float:
                quantiles.push_back(item.as_float());
                return absl::OkStatus();
            case Value::Type::Int:
                quantiles.push_back(static_cast<double>(item.as_int()));
                return absl::OkStatus();
            case Value::Type::UInt:
                quantiles.push_back(static_cast<double>(item.as_uint()));
                return absl::OkStatus();
            default:
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " `", property, "` must be a number or array of numbers"));
        }
    };
    if (value.type() == Value::Type::Array) {
        quantiles.reserve(value.as_array().elements.size());
        for (const auto& item : value.as_array().elements) {
            auto status = append_quantile(item);
            if (!status.ok()) {
                return status;
            }
        }
    } else {
        auto status = append_quantile(value);
        if (!status.ok()) {
            return status;
        }
    }
    if (quantiles.empty()) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must not be empty"));
    }
    return quantiles;
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

std::optional<int64_t> parse_rfc3339_seconds(const std::string& literal);
std::string format_rfc3339_seconds(int64_t seconds);

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
    const auto row_seconds = parse_rfc3339_seconds(*row_time);
    const auto start_seconds = start.has_value() ? parse_rfc3339_seconds(*start) : std::nullopt;
    const auto stop_seconds = stop.has_value() ? parse_rfc3339_seconds(*stop) : std::nullopt;
    if (row_seconds.has_value() && (!start.has_value() || start_seconds.has_value()) &&
        (!stop.has_value() || stop_seconds.has_value())) {
        if (start_seconds.has_value() && *row_seconds < *start_seconds) {
            return false;
        }
        if (stop_seconds.has_value() && *row_seconds >= *stop_seconds) {
            return false;
        }
        return true;
    }
    if (start.has_value() && *row_time < *start) {
        return false;
    }
    if (stop.has_value() && *row_time >= *stop) {
        return false;
    }
    return true;
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

std::pair<std::shared_ptr<ObjectValue>, std::string> clone_row_with_group_and_key(
    const ObjectValue& row, const std::vector<std::string>& columns) {
    std::vector<std::pair<std::string, Value>> group_props;
    group_props.reserve(columns.size());
    std::string key;
    key.reserve(columns.size() * 16);
    for (const auto& column : columns) {
        const Value* value = row.lookup(column);
        if (value != nullptr) {
            group_props.emplace_back(column, *value);
            absl::StrAppend(&key, column, "=");
            if (value->type() == Value::Type::String) {
                absl::StrAppend(&key, value->as_string());
            } else if (value->type() == Value::Type::Time) {
                absl::StrAppend(&key, value->as_time().literal);
            } else {
                absl::StrAppend(&key, value->string());
            }
        } else {
            absl::StrAppend(&key, column, "=<missing>");
        }
        key.push_back('\n');
    }
    Value group_value = Value::object(std::move(group_props));
    auto grouped = object_with_upserted_property(row, "_group", group_value);
    return {std::make_shared<ObjectValue>(grouped.as_object()), std::move(key)};
}

std::vector<std::string> all_visible_columns_in_order(const TableValue& table) {
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

std::vector<TableChunk> clone_table_chunks(const TableValue& table) { return table.tables; }

Value table_with_chunks_like(const TableValue& table, std::vector<TableChunk> chunks) {
    return Value::table_stream(table.bucket, std::move(chunks), table.range_start, table.range_stop,
                               table.result_name);
}

enum class EmptyChunkPolicy {
    Keep,
    Drop,
};

template <typename RowFn>
absl::StatusOr<Value> transform_rows_preserving_chunks(
    const TableValue& table,
    RowFn&& row_fn,
    EmptyChunkPolicy empty_policy = EmptyChunkPolicy::Keep) {
    std::vector<TableChunk> chunks;
    chunks.reserve(table.table_count());
    for (const auto& chunk : table.tables) {
        TableChunk next;
        next.rows.reserve(chunk.rows.size());
        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
            auto next_row_or = row_fn(*row);
            if (!next_row_or.ok()) {
                return next_row_or.status();
            }
            if (*next_row_or != nullptr) {
                next.rows.push_back(*next_row_or);
            }
        }
        if (next.rows.empty()) {
            next.group_key = chunk.group_key;
            next.columns = chunk.columns;
        }
        if (empty_policy == EmptyChunkPolicy::Keep || !next.rows.empty()) {
            chunks.push_back(std::move(next));
        }
    }
    return table_with_chunks_like(table, std::move(chunks));
}

Value slice_table_like(const TableValue& table,
                       std::function<std::pair<size_t, size_t>(size_t)> bounds_fn) {
    std::vector<TableChunk> chunks;
    chunks.reserve(table.table_count());
    for (const auto& chunk : table.tables) {
        const auto [begin, end] = bounds_fn(chunk.rows.size());
        TableChunk next;
        if (begin < end && begin < chunk.rows.size()) {
            next.rows.reserve(end - begin);
            for (size_t i = begin; i < end; ++i) {
                if (chunk.rows[i] != nullptr) {
                    next.rows.push_back(chunk.rows[i]);
                }
            }
        }
        if (next.rows.empty()) {
            next.group_key = chunk.group_key;
            next.columns = chunk.columns;
        }
        chunks.push_back(std::move(next));
    }
    return table_with_chunks_like(table, std::move(chunks));
}

std::shared_ptr<ObjectValue> materialize_group_count_row(const TableChunk& chunk,
                                                         const std::string& column,
                                                         int64_t count) {
    std::vector<std::pair<std::string, Value>> properties;
    if (chunk.group_key != nullptr) {
        for (const auto& [name, value] : chunk.group_key->properties) {
            properties.emplace_back(name, value);
        }
        properties.emplace_back("_group",
                                Value::object(std::make_shared<ObjectValue>(*chunk.group_key)));
    }
    properties.emplace_back(column, Value::integer(count));
    return std::make_shared<ObjectValue>(std::move(properties));
}

std::shared_ptr<ObjectValue> materialize_group_value_row(const TableChunk& chunk,
                                                         const std::string& column,
                                                         Value value) {
    std::vector<std::pair<std::string, Value>> properties;
    if (chunk.group_key != nullptr) {
        for (const auto& [name, group_value] : chunk.group_key->properties) {
            properties.emplace_back(name, group_value);
        }
        properties.emplace_back("_group",
                                Value::object(std::make_shared<ObjectValue>(*chunk.group_key)));
    }
    properties.emplace_back(column, std::move(value));
    return std::make_shared<ObjectValue>(std::move(properties));
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

std::optional<std::string> mapped_column_name(
    const std::vector<std::pair<std::string, std::string>>& mappings, const std::string& key) {
    for (const auto& [from, to] : mappings) {
        if (from == key) {
            return to;
        }
    }
    return std::nullopt;
}

void append_value_key_fragment(std::string* out, const Value& value) {
    if (value.type() == Value::Type::String) {
        absl::StrAppend(out, value.as_string());
        return;
    }
    if (value.type() == Value::Type::Time) {
        absl::StrAppend(out, value.as_time().literal);
        return;
    }
    absl::StrAppend(out, value.string());
}

void append_internal_key_fragment(std::string* out, const Value* value) {
    if (value == nullptr) {
        out->push_back('!');
        return;
    }
    switch (value->type()) {
        case Value::Type::Null:
            out->push_back('N');
            return;
        case Value::Type::Bool:
            out->push_back('B');
            out->push_back(value->as_bool() ? '1' : '0');
            return;
        case Value::Type::Int:
            out->push_back('I');
            absl::StrAppend(out, value->as_int());
            out->push_back(';');
            return;
        case Value::Type::UInt:
            out->push_back('U');
            absl::StrAppend(out, value->as_uint());
            out->push_back(';');
            return;
        case Value::Type::Float:
            out->push_back('F');
            absl::StrAppend(out, value->as_float());
            out->push_back(';');
            return;
        case Value::Type::String: {
            out->push_back('S');
            const auto& payload = value->as_string();
            absl::StrAppend(out, payload.size());
            out->push_back(':');
            out->append(payload);
            return;
        }
        case Value::Type::Time: {
            out->push_back('T');
            const auto& payload = value->as_time().literal;
            absl::StrAppend(out, payload.size());
            out->push_back(':');
            out->append(payload);
            return;
        }
        case Value::Type::Duration: {
            out->push_back('D');
            const auto& payload = value->as_duration().literal;
            absl::StrAppend(out, payload.size());
            out->push_back(':');
            out->append(payload);
            return;
        }
        case Value::Type::Regex: {
            out->push_back('R');
            const auto& payload = value->as_regex().literal;
            absl::StrAppend(out, payload.size());
            out->push_back(':');
            out->append(payload);
            return;
        }
        case Value::Type::Array:
        case Value::Type::Object:
        case Value::Type::Table:
        case Value::Type::Function: {
            out->push_back('X');
            const std::string payload = value->string();
            absl::StrAppend(out, payload.size());
            out->push_back(':');
            out->append(payload);
            return;
        }
    }
}

struct PivotColumnIdentity {
    std::string cache_key;
    std::string name;
};

struct PivotRowProjection {
    std::vector<const Value*> row_key_values;
    std::vector<const Value*> column_key_values;
    std::vector<std::pair<std::string, Value>> passthrough_props;
    const Value* value = nullptr;
};

std::string row_identity_key_from_values(const std::vector<std::string>& columns,
                                         const std::vector<const Value*>& values) {
    std::string key;
    key.reserve(columns.size() * 16);
    for (size_t i = 0; i < columns.size(); ++i) {
        absl::StrAppend(&key, columns[i], "=");
        if (values[i] != nullptr) {
            append_value_key_fragment(&key, *values[i]);
        } else {
            absl::StrAppend(&key, "<missing>");
        }
        key.push_back('\n');
    }
    return key;
}

PivotColumnIdentity pivot_column_identity_from_values(const std::vector<const Value*>& values) {
    PivotColumnIdentity identity;
    identity.cache_key.reserve(values.size() * 12);
    identity.name.reserve(values.size() * 12);
    for (const Value* value : values) {
        append_internal_key_fragment(&identity.cache_key, value);
        identity.cache_key.push_back('\n');
        if (!identity.name.empty()) {
            identity.name.push_back('_');
        }
        if (value != nullptr) {
            append_value_key_fragment(&identity.name, *value);
        } else {
            absl::StrAppend(&identity.name, "null");
        }
    }
    return identity;
}

PivotRowProjection project_pivot_row(
    const ObjectValue& row,
    const std::unordered_map<std::string, size_t>& row_key_indexes,
    const std::unordered_map<std::string, size_t>& column_key_indexes,
    const std::string& value_column) {
    PivotRowProjection projection;
    projection.row_key_values.resize(row_key_indexes.size(), nullptr);
    projection.column_key_values.resize(column_key_indexes.size(), nullptr);
    projection.passthrough_props.reserve(row.properties.size());
    for (const auto& [key, value] : row.properties) {
        if (const auto row_key = row_key_indexes.find(key); row_key != row_key_indexes.end()) {
            projection.row_key_values[row_key->second] = &value;
            continue;
        }
        if (const auto column_key = column_key_indexes.find(key);
            column_key != column_key_indexes.end()) {
            projection.column_key_values[column_key->second] = &value;
            continue;
        }
        if (key == value_column) {
            projection.value = &value;
            continue;
        }
        projection.passthrough_props.emplace_back(key, value);
    }
    return projection;
}

std::vector<std::string> visible_columns_in_chunk(const TableChunk& chunk) {
    std::vector<std::string> columns;
    std::unordered_set<std::string> seen;
    for (const auto& row : chunk.rows) {
        if (row == nullptr) {
            continue;
        }
        for (const auto& [key, value] : row->properties) {
            (void)value;
            if (key == "_group") {
                continue;
            }
            if (seen.insert(key).second) {
                columns.push_back(key);
            }
        }
    }
    return columns;
}

std::shared_ptr<ObjectValue> chunk_group_object(const TableChunk& chunk) {
    if (chunk.group_key != nullptr) {
        return std::make_shared<ObjectValue>(*chunk.group_key);
    }
    for (const auto& row : chunk.rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* group = row->lookup("_group");
        if (group != nullptr && group->type() == Value::Type::Object) {
            return std::make_shared<ObjectValue>(group->as_object());
        }
    }
    return std::make_shared<ObjectValue>(std::vector<std::pair<std::string, Value>>{});
}

struct PivotOutputRow {
    std::shared_ptr<ObjectValue> row;
    std::unordered_map<std::string, size_t> property_indexes;
};

void upsert_property_with_index(PivotOutputRow& output_row, const std::string& key, Value value) {
    const auto existing = output_row.property_indexes.find(key);
    if (existing != output_row.property_indexes.end()) {
        output_row.row->properties[existing->second].second = std::move(value);
        return;
    }
    output_row.property_indexes.emplace(key, output_row.row->properties.size());
    output_row.row->properties.emplace_back(key, std::move(value));
}

absl::StatusOr<double> quantile_for_sorted_values(const std::vector<double>& values, double q) {
    if (values.empty()) {
        return absl::InvalidArgumentError("quantile expects at least one numeric value");
    }
    if (q < 0.0 || q > 1.0) {
        return absl::InvalidArgumentError("quantile `q` must be between 0.0 and 1.0");
    }
    if (values.size() == 1) {
        return values.front();
    }
    const double position = q * static_cast<double>(values.size() - 1);
    const size_t lower_index = static_cast<size_t>(std::floor(position));
    const size_t upper_index = static_cast<size_t>(std::ceil(position));
    const double lower = values[lower_index];
    const double upper = values[upper_index];
    const double weight = position - static_cast<double>(lower_index);
    return lower + (upper - lower) * weight;
}

absl::StatusOr<std::vector<double>> numeric_values_for_chunk(const TableChunk& chunk,
                                                             const std::string& name,
                                                             const std::string& column) {
    std::vector<double> values;
    values.reserve(chunk.rows.size());
    for (const auto& row : chunk.rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* value = row->lookup(column);
        if (value == nullptr || value->is_null()) {
            continue;
        }
        if (!is_numeric_value(*value)) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", column, "` must be numeric"));
        }
        values.push_back(numeric_value(*value));
    }
    return values;
}

std::shared_ptr<ObjectValue> window_group_object(const TableChunk& chunk,
                                                 const std::string& start_column,
                                                 int64_t start_seconds,
                                                 const std::string& stop_column,
                                                 int64_t stop_seconds) {
    auto group = chunk_group_object(chunk);
    Value updated = object_with_upserted_property(
        *group, start_column, Value::time(format_rfc3339_seconds(start_seconds)));
    updated = object_with_upserted_property(updated.as_object(), stop_column,
                                            Value::time(format_rfc3339_seconds(stop_seconds)));
    return std::make_shared<ObjectValue>(updated.as_object());
}

std::shared_ptr<ObjectValue> row_with_window_bounds(const ObjectValue& row,
                                                    const std::string& start_column,
                                                    int64_t start_seconds,
                                                    const std::string& stop_column,
                                                    int64_t stop_seconds,
                                                    const std::shared_ptr<ObjectValue>& group) {
    Value updated = object_with_upserted_property(
        row, start_column, Value::time(format_rfc3339_seconds(start_seconds)));
    updated = object_with_upserted_property(updated.as_object(), stop_column,
                                            Value::time(format_rfc3339_seconds(stop_seconds)));
    updated = object_with_upserted_property(updated.as_object(), "_group", Value::object(group));
    return std::make_shared<ObjectValue>(updated.as_object());
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
        static_cast<int>(civil.year()), static_cast<unsigned>(civil.month()),
        static_cast<unsigned>(civil.day()), static_cast<unsigned>(civil.hour()),
        static_cast<unsigned>(civil.minute()), static_cast<unsigned>(civil.second()));
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

absl::StatusOr<WindowLocation> parse_window_location_value(const Value& value,
                                                           const std::string& name,
                                                           const std::string& property) {
    if (value.type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be an object record"));
    }
    const auto& location = value.as_object();
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
            absl::StrCat(name, " `", property, "` with a named zone must use offset 0s"));
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

absl::StatusOr<WindowLocation> optional_window_location_property(const ObjectValue& object,
                                                                 const std::string& name) {
    const Value* location_value = object.lookup("location");
    if (location_value == nullptr) {
        return WindowLocation{};
    }
    return parse_window_location_value(*location_value, name, "location");
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
    rows.reserve(table.rows.size());
    for (const auto& row : table.rows) {
        if (row == nullptr) {
            continue;
        }
        auto keep_or = ExpressionEvaluator::Invoke(predicate, {Value::object(row)});
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

std::shared_ptr<ObjectValue> aggregate_window_output_row(
    const std::shared_ptr<ObjectValue>& base_row,
    const std::string& aggregate_column,
    Value aggregate_value,
    const std::optional<int64_t>& window_start,
    const std::optional<int64_t>& window_stop,
    const std::string& time_dst_column,
    const std::optional<int64_t>& time_src_seconds) {
    std::vector<std::pair<std::string, Value>> props = base_row->properties;
    props.reserve(base_row->properties.size() + 3);

    std::optional<size_t> aggregate_index;
    std::optional<size_t> start_index;
    std::optional<size_t> stop_index;
    std::optional<size_t> time_index;
    for (size_t i = 0; i < props.size(); ++i) {
        const std::string& key = props[i].first;
        if (key == aggregate_column) {
            aggregate_index = i;
        } else if (key == "_start") {
            start_index = i;
        } else if (key == "_stop") {
            stop_index = i;
        } else if (key == time_dst_column) {
            time_index = i;
        }
    }

    const auto upsert = [&](const std::string& key, std::optional<size_t>* index, Value value) {
        if (index->has_value()) {
            props[**index].second = std::move(value);
        } else {
            *index = props.size();
            props.emplace_back(key, std::move(value));
        }
    };

    upsert(aggregate_column, &aggregate_index, std::move(aggregate_value));
    if (window_start.has_value()) {
        upsert("_start", &start_index, Value::time(format_rfc3339_seconds(*window_start)));
    }
    if (window_stop.has_value()) {
        upsert("_stop", &stop_index, Value::time(format_rfc3339_seconds(*window_stop)));
    }
    if (time_src_seconds.has_value()) {
        upsert(time_dst_column, &time_index,
               Value::time(format_rfc3339_seconds(*time_src_seconds)));
    }
    return std::make_shared<ObjectValue>(std::move(props));
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

} // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

} // namespace pl
