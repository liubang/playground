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

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/execution/materializer.h"
#include "cpp/pl/flux/runtime/runtime_builtin_package.h"
#include "cpp/pl/flux/runtime/runtime_builtin_table_helpers.h"
#include "cpp/pl/flux/runtime/runtime_builtin_universe.h"
#include "cpp/pl/flux/runtime/runtime_eval.h"

namespace pl::flux {
namespace {
using namespace detail;

absl::StatusOr<const TableValue*> materialized_table_ref(const TableValue& table, Value* storage) {
    if (table.materialized) {
        return &table;
    }
    Value value = Value::table_plan(
        table.bucket, table.plan, table.range_start, table.range_stop, table.result_name);
    auto materialized_or = execution::MaterializeValue(std::move(value));
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    *storage = std::move(*materialized_or);
    return &storage->as_table();
}

std::optional<plan::JoinMethod> plan_join_method(const std::string& method) {
    if (method == "inner") {
        return plan::JoinMethod::Inner;
    }
    if (method == "left") {
        return plan::JoinMethod::Left;
    }
    if (method == "right") {
        return plan::JoinMethod::Right;
    }
    if (method == "full") {
        return plan::JoinMethod::Full;
    }
    return std::nullopt;
}

std::optional<Value> lazy_join_with_column_keys(const TableValue& left,
                                                const TableValue& right,
                                                const std::string& left_name,
                                                const std::string& right_name,
                                                const std::vector<std::string>& on_columns,
                                                const std::string& method,
                                                const FunctionValue* as_fn) {
    const auto plan_method = plan_join_method(method);
    if (left.materialized || right.materialized || left.plan == nullptr || right.plan == nullptr ||
        !plan_method.has_value() || as_fn != nullptr) {
        return std::nullopt;
    }
    return Value::table_plan(
        left.bucket.empty() ? right.bucket : left.bucket,
        plan::MakeJoin(left.plan, right.plan, on_columns, *plan_method, left_name, right_name),
        left.range_start.has_value() ? left.range_start : right.range_start,
        left.range_stop.has_value() ? left.range_stop : right.range_stop);
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

std::string joined_property_name(const std::string& table_name, const std::string& column) {
    return column + "_" + table_name;
}

std::unordered_set<std::string> overlapping_join_columns(
    const std::vector<std::string>& left_columns,
    const std::vector<std::string>& right_columns,
    const std::unordered_set<std::string>& on_columns) {
    std::unordered_set<std::string> right_column_set(right_columns.begin(), right_columns.end());
    std::unordered_set<std::string> overlap;
    overlap.reserve(left_columns.size());
    for (const auto& column : left_columns) {
        if (on_columns.count(column) != 0 || right_column_set.count(column) == 0) {
            continue;
        }
        overlap.insert(column);
    }
    return overlap;
}

std::optional<std::string> join_row_key(const ObjectValue& row,
                                        const std::vector<std::string>& on_columns) {
    std::string key;
    key.reserve(on_columns.size() * 16);
    for (const auto& column : on_columns) {
        const Value* value = row.lookup(column);
        if (value == nullptr || value->is_null()) {
            return std::nullopt;
        }
        absl::StrAppend(&key, column, "=");
        append_value_key_fragment(&key, *value);
        key.push_back('\n');
    }
    return key;
}

std::string chunk_group_key(const TableChunk& chunk) {
    if (chunk.group_key != nullptr) {
        return chunk.group_key->string();
    }
    for (const auto& row : chunk.rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* group = row->lookup("_group");
        return group == nullptr ? "" : group->string();
    }
    return "";
}

std::vector<std::pair<std::string, Value>> join_group_properties(
    const ObjectValue* left,
    const ObjectValue* right,
    const std::string& left_name,
    const std::string& right_name,
    const std::unordered_set<std::string>& on_columns,
    const std::unordered_set<std::string>& overlapping_columns) {
    std::vector<std::pair<std::string, Value>> props;
    std::unordered_set<std::string> inserted;

    auto append_group = [&](const ObjectValue* row, const std::string& table_name) {
        if (row == nullptr) {
            return;
        }
        const Value* group = row->lookup("_group");
        if (group == nullptr || group->type() != Value::Type::Object) {
            return;
        }
        for (const auto& [key, value] : group->as_object().properties) {
            std::string output_key = key;
            if (on_columns.count(key) != 0) {
                if (inserted.insert(key).second) {
                    props.emplace_back(key, value);
                }
                continue;
            }
            if (overlapping_columns.count(key) != 0) {
                output_key = joined_property_name(table_name, key);
            }
            if (inserted.insert(output_key).second) {
                props.emplace_back(output_key, value);
            }
        }
    };

    append_group(left, left_name);
    append_group(right, right_name);
    return props;
}

struct JoinChunkIndex {
    const TableChunk* chunk = nullptr;
    std::vector<std::string> columns;
    std::unordered_map<std::string, std::vector<const ObjectValue*>> rows_by_key;
};

std::shared_ptr<ObjectValue> join_rows(const std::string& left_name,
                                       const ObjectValue* left,
                                       const std::vector<std::string>& left_columns,
                                       const std::string& right_name,
                                       const ObjectValue* right,
                                       const std::vector<std::string>& right_columns,
                                       const std::vector<std::string>& on_columns,
                                       const std::unordered_set<std::string>& on_column_set,
                                       const std::unordered_set<std::string>& overlapping_columns) {
    std::vector<std::pair<std::string, Value>> props;
    auto group_props = join_group_properties(
        left, right, left_name, right_name, on_column_set, overlapping_columns);
    props.reserve(left_columns.size() + right_columns.size() + group_props.size() + 1);
    for (const auto& [key, value] : group_props) {
        props.emplace_back(key, value);
    }
    for (const auto& column : on_columns) {
        const Value* value = left != nullptr ? left->lookup(column) : nullptr;
        if (value == nullptr && right != nullptr) {
            value = right->lookup(column);
        }
        if (value != nullptr) {
            const bool already_present =
                std::any_of(props.begin(), props.end(), [&](const auto& property) {
                    return property.first == column;
                });
            if (!already_present) {
                props.emplace_back(column, *value);
            }
        }
    }

    auto append_side = [&](const std::string& table_name,
                           const ObjectValue* row,
                           const std::vector<std::string>& columns) {
        for (const auto& column : columns) {
            if (column == "_group" || on_column_set.count(column) != 0) {
                continue;
            }
            const std::string output_key = overlapping_columns.count(column) != 0
                                               ? joined_property_name(table_name, column)
                                               : column;
            const bool already_present =
                std::any_of(props.begin(), props.end(), [&](const auto& property) {
                    return property.first == output_key;
                });
            if (already_present) {
                continue;
            }
            const Value* value = row != nullptr ? row->lookup(column) : nullptr;
            props.emplace_back(output_key, value != nullptr ? *value : Value::null());
        }
    };

    append_side(left_name, left, left_columns);
    append_side(right_name, right, right_columns);
    if (!group_props.empty()) {
        props.emplace_back("_group", Value::object(std::move(group_props)));
    }
    return std::make_shared<ObjectValue>(std::move(props));
}

std::shared_ptr<ObjectValue> null_row_for_columns(const std::vector<std::string>& columns) {
    std::vector<std::pair<std::string, Value>> props;
    props.reserve(columns.size());
    for (const auto& column : columns) {
        props.emplace_back(column, Value::null());
    }
    return std::make_shared<ObjectValue>(std::move(props));
}

absl::StatusOr<std::shared_ptr<ObjectValue>> join_as_row(
    const FunctionValue* as_fn,
    const ObjectValue* left,
    const std::vector<std::string>& left_columns,
    const ObjectValue* right,
    const std::vector<std::string>& right_columns) {
    if (as_fn == nullptr) {
        return absl::InvalidArgumentError("join package requires `as` when using predicate `on`");
    }
    auto left_null = null_row_for_columns(left_columns);
    auto right_null = null_row_for_columns(right_columns);
    auto row_or = ExpressionEvaluator::Invoke(
        Value::function(std::make_shared<FunctionValue>(*as_fn)),
        {Value::object(left != nullptr ? std::make_shared<ObjectValue>(*left) : left_null),
         Value::object(right != nullptr ? std::make_shared<ObjectValue>(*right) : right_null)});
    if (!row_or.ok()) {
        return row_or.status();
    }
    if (row_or->type() != Value::Type::Object) {
        return absl::InvalidArgumentError("join package `as` must return an object");
    }
    return std::make_shared<ObjectValue>(row_or->as_object());
}

absl::StatusOr<bool> join_predicate_matches(const FunctionValue& on_fn,
                                            const ObjectValue& left,
                                            const ObjectValue& right) {
    auto keep_or =
        ExpressionEvaluator::Invoke(Value::function(std::make_shared<FunctionValue>(on_fn)),
                                    {Value::object(std::make_shared<ObjectValue>(left)),
                                     Value::object(std::make_shared<ObjectValue>(right))});
    if (!keep_or.ok()) {
        return keep_or.status();
    }
    if (keep_or->type() != Value::Type::Bool) {
        return absl::InvalidArgumentError("join package `on` must return a boolean");
    }
    return keep_or->as_bool();
}

absl::StatusOr<Value> join_with_column_keys(const TableValue& left_table,
                                            const TableValue& right_table,
                                            const std::string& left_name,
                                            const std::string& right_name,
                                            const std::vector<std::string>& on_columns,
                                            const std::string& method,
                                            const FunctionValue* as_fn) {
    const std::unordered_set<std::string> on_column_set(on_columns.begin(), on_columns.end());
    const auto left_columns = all_visible_columns_in_order(left_table);
    const auto right_columns = all_visible_columns_in_order(right_table);
    const auto overlapping_columns =
        overlapping_join_columns(left_columns, right_columns, on_column_set);

    std::unordered_map<std::string, std::vector<JoinChunkIndex>> right_chunks_by_group;
    right_chunks_by_group.reserve(right_table.table_count());
    for (const auto& right_chunk : right_table.tables) {
        JoinChunkIndex index;
        index.chunk = &right_chunk;
        index.columns = visible_columns_in_chunk(right_chunk);
        index.rows_by_key.reserve(right_chunk.rows.size());
        for (const auto& right_row : right_chunk.rows) {
            if (right_row == nullptr) {
                continue;
            }
            const auto key = join_row_key(*right_row, on_columns);
            if (!key.has_value()) {
                continue;
            }
            index.rows_by_key[*key].push_back(right_row.get());
        }
        right_chunks_by_group[chunk_group_key(right_chunk)].push_back(std::move(index));
    }

    std::unordered_set<std::string> processed_groups;
    std::vector<TableChunk> output_chunks;

    auto emit_group = [&](const std::vector<const TableChunk*>& left_chunks,
                          std::vector<JoinChunkIndex>* right_indexes) -> absl::Status {
        TableChunk output_chunk;
        std::unordered_set<const ObjectValue*> matched_right_rows;
        for (const auto* left_chunk : left_chunks) {
            for (const auto& left_row : left_chunk->rows) {
                if (left_row == nullptr) {
                    continue;
                }
                bool matched = false;
                const auto key = join_row_key(*left_row, on_columns);
                if (key.has_value()) {
                    for (const auto& right_index : *right_indexes) {
                        const auto matches = right_index.rows_by_key.find(*key);
                        if (matches == right_index.rows_by_key.end()) {
                            continue;
                        }
                        matched = true;
                        for (const auto* right_row : matches->second) {
                            matched_right_rows.insert(right_row);
                            if (as_fn != nullptr) {
                                auto row_or = join_as_row(
                                    as_fn, left_row.get(), left_columns, right_row, right_columns);
                                if (!row_or.ok()) {
                                    return row_or.status();
                                }
                                output_chunk.rows.push_back(*row_or);
                            } else {
                                output_chunk.rows.push_back(join_rows(left_name,
                                                                      left_row.get(),
                                                                      left_columns,
                                                                      right_name,
                                                                      right_row,
                                                                      right_columns,
                                                                      on_columns,
                                                                      on_column_set,
                                                                      overlapping_columns));
                            }
                        }
                    }
                }
                if (!matched && (method == "left" || method == "full")) {
                    if (as_fn != nullptr) {
                        auto row_or = join_as_row(
                            as_fn, left_row.get(), left_columns, nullptr, right_columns);
                        if (!row_or.ok()) {
                            return row_or.status();
                        }
                        output_chunk.rows.push_back(*row_or);
                    } else {
                        output_chunk.rows.push_back(join_rows(left_name,
                                                              left_row.get(),
                                                              left_columns,
                                                              right_name,
                                                              nullptr,
                                                              right_columns,
                                                              on_columns,
                                                              on_column_set,
                                                              overlapping_columns));
                    }
                }
            }
        }

        if (method == "right" || method == "full") {
            for (const auto& right_index : *right_indexes) {
                for (const auto& right_row : right_index.chunk->rows) {
                    if (right_row == nullptr || matched_right_rows.count(right_row.get()) != 0) {
                        continue;
                    }
                    if (as_fn != nullptr) {
                        auto row_or = join_as_row(
                            as_fn, nullptr, left_columns, right_row.get(), right_columns);
                        if (!row_or.ok()) {
                            return row_or.status();
                        }
                        output_chunk.rows.push_back(*row_or);
                    } else {
                        output_chunk.rows.push_back(join_rows(left_name,
                                                              nullptr,
                                                              left_columns,
                                                              right_name,
                                                              right_row.get(),
                                                              right_columns,
                                                              on_columns,
                                                              on_column_set,
                                                              overlapping_columns));
                    }
                }
            }
        }

        if (!output_chunk.rows.empty()) {
            output_chunks.push_back(std::move(output_chunk));
        }
        return absl::OkStatus();
    };

    std::unordered_map<std::string, std::vector<const TableChunk*>> left_chunks_by_group;
    left_chunks_by_group.reserve(left_table.table_count());
    for (const auto& left_chunk : left_table.tables) {
        left_chunks_by_group[chunk_group_key(left_chunk)].push_back(&left_chunk);
    }

    for (const auto& [group_key, left_chunks] : left_chunks_by_group) {
        processed_groups.insert(group_key);
        auto right_it = right_chunks_by_group.find(group_key);
        if (right_it == right_chunks_by_group.end()) {
            if (method == "left" || method == "full") {
                std::vector<JoinChunkIndex> empty_right;
                auto status = emit_group(left_chunks, &empty_right);
                if (!status.ok()) {
                    return status;
                }
            }
            continue;
        }
        auto status = emit_group(left_chunks, &right_it->second);
        if (!status.ok()) {
            return status;
        }
    }

    if (method == "right" || method == "full") {
        for (auto& [group_key, right_indexes] : right_chunks_by_group) {
            if (processed_groups.count(group_key) != 0) {
                continue;
            }
            std::vector<const TableChunk*> empty_left;
            auto status = emit_group(empty_left, &right_indexes);
            if (!status.ok()) {
                return status;
            }
        }
    }

    return Value::table_stream(
        left_table.bucket.empty() ? right_table.bucket : left_table.bucket,
        std::move(output_chunks),
        left_table.range_start.has_value() ? left_table.range_start : right_table.range_start,
        left_table.range_stop.has_value() ? left_table.range_stop : right_table.range_stop);
}

absl::StatusOr<Value> join_with_predicate(const TableValue& left_table,
                                          const TableValue& right_table,
                                          const FunctionValue& on_fn,
                                          const FunctionValue& as_fn,
                                          const std::string& method) {
    const auto left_columns = all_visible_columns_in_order(left_table);
    const auto right_columns = all_visible_columns_in_order(right_table);

    std::unordered_map<std::string, std::vector<const TableChunk*>> right_chunks_by_group;
    right_chunks_by_group.reserve(right_table.table_count());
    for (const auto& right_chunk : right_table.tables) {
        right_chunks_by_group[chunk_group_key(right_chunk)].push_back(&right_chunk);
    }

    std::unordered_set<std::string> processed_groups;
    std::vector<TableChunk> output_chunks;

    auto emit_group = [&](const std::vector<const TableChunk*>& left_chunks,
                          const std::vector<const TableChunk*>& right_chunks) -> absl::Status {
        std::unordered_set<const ObjectValue*> matched_right_rows;
        TableChunk output_chunk;
        for (const auto* left_chunk : left_chunks) {
            for (const auto& left_row : left_chunk->rows) {
                if (left_row == nullptr) {
                    continue;
                }
                bool matched = false;
                for (const auto* right_chunk : right_chunks) {
                    for (const auto& right_row : right_chunk->rows) {
                        if (right_row == nullptr) {
                            continue;
                        }
                        auto matched_or = join_predicate_matches(on_fn, *left_row, *right_row);
                        if (!matched_or.ok()) {
                            return matched_or.status();
                        }
                        if (!*matched_or) {
                            continue;
                        }
                        matched = true;
                        matched_right_rows.insert(right_row.get());
                        auto row_or = join_as_row(
                            &as_fn, left_row.get(), left_columns, right_row.get(), right_columns);
                        if (!row_or.ok()) {
                            return row_or.status();
                        }
                        output_chunk.rows.push_back(*row_or);
                    }
                }
                if (!matched && (method == "left" || method == "full")) {
                    auto row_or =
                        join_as_row(&as_fn, left_row.get(), left_columns, nullptr, right_columns);
                    if (!row_or.ok()) {
                        return row_or.status();
                    }
                    output_chunk.rows.push_back(*row_or);
                }
            }
        }
        if (method == "right" || method == "full") {
            for (const auto* right_chunk : right_chunks) {
                for (const auto& right_row : right_chunk->rows) {
                    if (right_row == nullptr || matched_right_rows.count(right_row.get()) != 0) {
                        continue;
                    }
                    auto row_or =
                        join_as_row(&as_fn, nullptr, left_columns, right_row.get(), right_columns);
                    if (!row_or.ok()) {
                        return row_or.status();
                    }
                    output_chunk.rows.push_back(*row_or);
                }
            }
        }
        if (!output_chunk.rows.empty()) {
            output_chunks.push_back(std::move(output_chunk));
        }
        return absl::OkStatus();
    };

    std::unordered_map<std::string, std::vector<const TableChunk*>> left_chunks_by_group;
    left_chunks_by_group.reserve(left_table.table_count());
    for (const auto& left_chunk : left_table.tables) {
        left_chunks_by_group[chunk_group_key(left_chunk)].push_back(&left_chunk);
    }

    for (const auto& [group_key, left_chunks] : left_chunks_by_group) {
        processed_groups.insert(group_key);
        const auto right_it = right_chunks_by_group.find(group_key);
        std::vector<const TableChunk*> empty_right;
        const auto& right_chunks =
            right_it == right_chunks_by_group.end() ? empty_right : right_it->second;
        auto status = emit_group(left_chunks, right_chunks);
        if (!status.ok()) {
            return status;
        }
    }

    if (method == "right" || method == "full") {
        for (const auto& [group_key, right_chunks] : right_chunks_by_group) {
            if (processed_groups.count(group_key) != 0) {
                continue;
            }
            std::vector<const TableChunk*> empty_left;
            auto status = emit_group(empty_left, right_chunks);
            if (!status.ok()) {
                return status;
            }
        }
    }

    return Value::table_stream(
        left_table.bucket.empty() ? right_table.bucket : left_table.bucket,
        std::move(output_chunks),
        left_table.range_start.has_value() ? left_table.range_start : right_table.range_start,
        left_table.range_stop.has_value() ? left_table.range_stop : right_table.range_stop);
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
    std::string method = "inner";
    if (const Value* method_value = (*object_or)->lookup("method"); method_value != nullptr) {
        if (method_value->type() != Value::Type::String) {
            return absl::InvalidArgumentError("join `method` must be a string");
        }
        method = method_value->as_string();
    }
    if (method != "inner" && method != "left" && method != "right" && method != "full") {
        return absl::InvalidArgumentError(
            "join `method` must be one of \"inner\", \"left\", \"right\", or \"full\"");
    }
    auto lazy = lazy_join_with_column_keys(*(*tables_or)[0].second,
                                           *(*tables_or)[1].second,
                                           (*tables_or)[0].first,
                                           (*tables_or)[1].first,
                                           *on_or,
                                           method,
                                           nullptr);
    if (lazy.has_value()) {
        return std::move(*lazy);
    }

    std::vector<Value> materialized_tables;
    materialized_tables.reserve(tables_or->size());
    for (auto& [name, table] : *tables_or) {
        (void)name;
        if (table->materialized) {
            continue;
        }
        auto materialized_or = materialized_table_ref(*table, &materialized_tables.emplace_back());
        if (!materialized_or.ok()) {
            return materialized_or.status();
        }
        table = *materialized_or;
    }

    auto result_or = join_with_column_keys(*(*tables_or)[0].second,
                                           *(*tables_or)[1].second,
                                           (*tables_or)[0].first,
                                           (*tables_or)[1].first,
                                           *on_or,
                                           method,
                                           nullptr);
    if (!result_or.ok()) {
        return result_or.status();
    }
    std::vector<const TableValue*> inputs{(*tables_or)[0].second, (*tables_or)[1].second};
    return with_materialization_barrier(std::move(*result_or), inputs, "join");
}

absl::StatusOr<Value> builtin_join_package_method(const std::vector<Value>& args,
                                                  const std::string& method) {
    const std::string name = absl::StrCat("join.", method);
    auto object_or = require_object_argument(args, name);
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto left_or = require_table_property(**object_or, name, "left");
    if (!left_or.ok()) {
        return left_or.status();
    }
    auto right_or = require_table_property(**object_or, name, "right");
    if (!right_or.ok()) {
        return right_or.status();
    }
    auto on_value_or = require_object_property(**object_or, name, "on");
    if (!on_value_or.ok()) {
        return on_value_or.status();
    }

    const Value* as_value = (*object_or)->lookup("as");
    const FunctionValue* as_fn = nullptr;
    if (as_value != nullptr) {
        if (as_value->type() != Value::Type::Function) {
            return absl::InvalidArgumentError(absl::StrCat(name, " `as` must be a function"));
        }
        as_fn = &as_value->as_function();
    }

    std::optional<std::vector<std::string>> on_columns;
    std::string left_name;
    std::string right_name;
    if ((*on_value_or)->type() == Value::Type::Array) {
        auto on_or = string_array_value(**on_value_or, name, "on");
        if (!on_or.ok()) {
            return on_or.status();
        }
        auto left_name_or = optional_string_property(**object_or, name, "leftName", "left");
        if (!left_name_or.ok()) {
            return left_name_or.status();
        }
        auto right_name_or = optional_string_property(**object_or, name, "rightName", "right");
        if (!right_name_or.ok()) {
            return right_name_or.status();
        }
        on_columns = std::move(*on_or);
        left_name = std::move(*left_name_or);
        right_name = std::move(*right_name_or);
        auto lazy = lazy_join_with_column_keys(
            **left_or, **right_or, left_name, right_name, *on_columns, method, as_fn);
        if (lazy.has_value()) {
            return std::move(*lazy);
        }
    }

    Value left_materialized;
    auto left_materialized_or = materialized_table_ref(**left_or, &left_materialized);
    if (!left_materialized_or.ok()) {
        return left_materialized_or.status();
    }
    left_or = *left_materialized_or;
    Value right_materialized;
    auto right_materialized_or = materialized_table_ref(**right_or, &right_materialized);
    if (!right_materialized_or.ok()) {
        return right_materialized_or.status();
    }
    right_or = *right_materialized_or;

    if ((*on_value_or)->type() == Value::Type::Array) {
        auto result_or = join_with_column_keys(
            **left_or, **right_or, left_name, right_name, *on_columns, method, as_fn);
        if (!result_or.ok()) {
            return result_or.status();
        }
        std::vector<const TableValue*> inputs{*left_or, *right_or};
        return with_materialization_barrier(std::move(*result_or), inputs, name);
    }
    if ((*on_value_or)->type() == Value::Type::Function) {
        if (as_fn == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " requires `as` when `on` is a predicate function"));
        }
        auto result_or = join_with_predicate(
            **left_or, **right_or, (*on_value_or)->as_function(), *as_fn, method);
        if (!result_or.ok()) {
            return result_or.status();
        }
        std::vector<const TableValue*> inputs{*left_or, *right_or};
        return with_materialization_barrier(std::move(*result_or), inputs, name);
    }
    return absl::InvalidArgumentError(absl::StrCat(name, " `on` must be an array or function"));
}

Value make_join_package() {
    return Value::object({
        {"path", Value::string("join")},
        {"inner",
         make_builtin_value("join.inner",
                            [](const std::vector<Value>& args) {
                                return builtin_join_package_method(args, "inner");
                            })},
        {"left",
         make_builtin_value("join.left",
                            [](const std::vector<Value>& args) {
                                return builtin_join_package_method(args, "left");
                            })},
        {"right",
         make_builtin_value("join.right",
                            [](const std::vector<Value>& args) {
                                return builtin_join_package_method(args, "right");
                            })},
        {"full",
         make_builtin_value("join.full",
                            [](const std::vector<Value>& args) {
                                return builtin_join_package_method(args, "full");
                            })},
    });
}

} // namespace

bool InstallKnownUniverseJoinBuiltin(Environment& env, const std::string& name) {
    if (name == "join") {
        install_builtin(env, "join", builtin_join);
        return true;
    }
    return false;
}

namespace builtin {

void RegisterJoinStdlibPackage() {
    RegisterPackage("join", make_join_package);
}

} // namespace builtin

} // namespace pl::flux
