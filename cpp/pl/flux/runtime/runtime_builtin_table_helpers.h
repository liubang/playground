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
// Created: 2026/04/25 11:09

#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/runtime/runtime_env.h"
#include "cpp/pl/flux/runtime/runtime_eval.h"

namespace pl::flux::detail {

inline absl::StatusOr<const ArrayValue*> require_array_argument(const std::vector<Value>& args,
                                                                const std::string& name) {
    if (args.size() != 1 || args[0].type() != Value::Type::Array) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " expects exactly one array argument"));
    }
    return &args[0].as_array();
}

inline absl::StatusOr<const ObjectValue*> require_object_argument(const std::vector<Value>& args,
                                                                  const std::string& name) {
    if (args.size() != 1 || args[0].type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " expects exactly one object argument"));
    }
    return &args[0].as_object();
}

inline absl::StatusOr<const Value*> require_object_property(const ObjectValue& object,
                                                            const std::string& object_name,
                                                            const std::string& property) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return absl::InvalidArgumentError(absl::StrCat(object_name, " requires `", property, "`"));
    }
    return value;
}

inline absl::StatusOr<const ArrayValue*> require_array_property(const ObjectValue& object,
                                                                const std::string& name,
                                                                const std::string& property);

inline absl::StatusOr<std::vector<std::shared_ptr<ObjectValue>>> require_table_rows(
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

inline absl::StatusOr<const TableValue*> require_table_property(const ObjectValue& object,
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

inline absl::StatusOr<std::vector<const TableValue*>> require_table_array_property(
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

inline Value with_materialization_barrier(Value value,
                                          const TableValue& input,
                                          const std::string& builtin) {
    if (input.plan != nullptr) {
        value.as_table_mut().plan =
            plan::MakeMaterializeBarrier(input.plan, "unsupported lazy builtin", builtin);
    }
    return value;
}

inline Value with_materialization_barrier(Value value,
                                          const std::vector<const TableValue*>& inputs,
                                          const std::string& builtin) {
    std::vector<std::shared_ptr<plan::PlanNode>> input_plans;
    input_plans.reserve(inputs.size());
    for (const auto* input : inputs) {
        if (input != nullptr && input->plan != nullptr) {
            input_plans.push_back(input->plan);
        }
    }
    if (!input_plans.empty()) {
        value.as_table_mut().plan = plan::MakeMaterializeBarrier(
            std::move(input_plans), "unsupported lazy builtin", builtin);
    }
    return value;
}

Value with_aggregate_plan(Value value,
                          const TableValue& input,
                          plan::AggregateFunction fn,
                          std::string column);
Value with_distinct_plan(Value value, const TableValue& input, std::string column);

inline absl::StatusOr<const ArrayValue*> require_array_property(const ObjectValue& object,
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

inline std::optional<std::string> optional_literal_property(const ObjectValue& object,
                                                            const std::string& property) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return std::nullopt;
    }
    return value->string();
}

inline absl::StatusOr<int64_t> integer_property(const ObjectValue& object,
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
        const uint64_t value = (*value_or)->as_uint();
        if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` overflows int64"));
        }
        return static_cast<int64_t>(value);
    }
    return absl::InvalidArgumentError(
        absl::StrCat(name, " `", property, "` must be an int or uint"));
}

inline absl::StatusOr<bool> optional_bool_property(const ObjectValue& object,
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

inline absl::StatusOr<std::string> optional_string_property(const ObjectValue& object,
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

inline absl::StatusOr<size_t> optional_index_property(const ObjectValue& object,
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

inline absl::StatusOr<std::string> string_property(const ObjectValue& object,
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

inline absl::StatusOr<std::vector<std::string>> string_array_value(const Value& value,
                                                                   const std::string& name,
                                                                   const std::string& property) {
    if (value.type() != Value::Type::Array) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be an array"));
    }
    std::vector<std::string> values;
    values.reserve(value.as_array().elements.size());
    for (const auto& item : value.as_array().elements) {
        if (item.type() != Value::Type::String) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must contain only strings"));
        }
        values.push_back(item.as_string());
    }
    return values;
}

inline absl::StatusOr<std::vector<std::string>> string_array_property(const ObjectValue& object,
                                                                      const std::string& name,
                                                                      const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    return string_array_value(**value_or, name, property);
}

inline absl::StatusOr<std::vector<std::string>> optional_string_array_property(
    const ObjectValue& object,
    const std::string& name,
    const std::string& property,
    std::vector<std::string> default_value) {
    if (object.lookup(property) == nullptr) {
        return default_value;
    }
    return string_array_property(object, name, property);
}

inline absl::StatusOr<std::vector<std::pair<std::string, std::string>>> string_map_property(
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

inline std::shared_ptr<ObjectValue> clone_row(const ObjectValue& row) {
    return std::make_shared<ObjectValue>(row);
}

inline Value object_with_upserted_property(const ObjectValue& object,
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

inline std::pair<std::shared_ptr<ObjectValue>, std::string> clone_row_with_group_and_key(
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

inline std::vector<std::string> all_visible_columns_in_order(const TableValue& table) {
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

inline std::vector<TableChunk> clone_table_chunks(const TableValue& table) {
    return table.tables;
}

inline Value table_with_chunks_like(const TableValue& table, std::vector<TableChunk> chunks) {
    return Value::table_stream(
        table.bucket, std::move(chunks), table.range_start, table.range_stop, table.result_name);
}

enum class EmptyChunkPolicy {
    Keep,
    Drop,
};

template <typename RowFn>
inline absl::StatusOr<Value> transform_rows_preserving_chunks(
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

inline Value slice_table_like(const TableValue& table,
                              const std::function<std::pair<size_t, size_t>(size_t)>& bounds_fn) {
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

inline void append_value_key_fragment(std::string* out, const Value& value) {
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

inline void append_internal_key_fragment(std::string* out, const Value* value) {
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

inline std::string row_identity_key_from_values(const std::vector<std::string>& columns,
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

inline PivotColumnIdentity pivot_column_identity_from_values(
    const std::vector<const Value*>& values) {
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

inline PivotRowProjection project_pivot_row(
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

inline std::vector<std::string> visible_columns_in_chunk(const TableChunk& chunk) {
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

inline std::shared_ptr<ObjectValue> chunk_group_object(const TableChunk& chunk) {
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

inline void upsert_property_with_index(PivotOutputRow& output_row,
                                       const std::string& key,
                                       Value value) {
    const auto existing = output_row.property_indexes.find(key);
    if (existing != output_row.property_indexes.end()) {
        output_row.row->properties[existing->second].second = std::move(value);
        return;
    }
    output_row.property_indexes.emplace(key, output_row.row->properties.size());
    output_row.row->properties.emplace_back(key, std::move(value));
}

inline double numeric_value(const Value& value) {
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

inline bool is_numeric_value(const Value& value) {
    return value.type() == Value::Type::Int || value.type() == Value::Type::UInt ||
           value.type() == Value::Type::Float;
}

inline int compare_values(const Value* lhs, const Value* rhs) {
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

inline std::optional<std::string> mapped_column_name(
    const std::vector<std::pair<std::string, std::string>>& mappings, const std::string& key) {
    for (const auto& [from, to] : mappings) {
        if (from == key) {
            return to;
        }
    }
    return std::nullopt;
}

inline std::string group_key_for_row(const ObjectValue& row) {
    const Value* group = row.lookup("_group");
    return group == nullptr ? "" : group->string();
}

inline std::vector<std::string> visible_columns_in_order(const TableValue& table) {
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

inline std::vector<std::string> group_columns_in_order(const TableValue& table) {
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

inline absl::StatusOr<std::vector<std::shared_ptr<ObjectValue>>> filter_rows_by_function(
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

inline Value make_builtin_value(const std::string& name,
                                FunctionValue::BuiltinCallback fn,
                                std::string pipe_param_name = {}) {
    auto callable = std::make_shared<FunctionValue>();
    callable->kind = FunctionValue::Kind::Builtin;
    callable->name = name;
    callable->pipe_param_name = std::move(pipe_param_name);
    callable->builtin = std::move(fn);
    return Value::function(std::move(callable));
}

inline void install_builtin(Environment& env,
                            const std::string& name,
                            FunctionValue::BuiltinCallback fn,
                            std::string pipe_param_name = {}) {
    env.define(name, make_builtin_value(name, std::move(fn), std::move(pipe_param_name)));
}

} // namespace pl::flux::detail
