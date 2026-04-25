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

#include "cpp/pl/flux/runtime_builtin_table_helpers.h"
#include "cpp/pl/flux/runtime_builtin_time_helpers.h"
#include "cpp/pl/flux/runtime_builtin_universe.h"

namespace pl {
namespace {

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

    auto ranged_or = transform_rows_preserving_chunks(
        **table_or, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            if (!row_matches_time_bounds(row, start, stop)) {
                return std::shared_ptr<ObjectValue>{};
            }
            return clone_row(row);
        });
    if (!ranged_or.ok()) {
        return ranged_or.status();
    }
    auto table = ranged_or->as_table();
    table.range_start = start;
    table.range_stop = stop;
    return Value::table_stream(table.bucket, std::move(table.tables), table.range_start,
                               table.range_stop, table.result_name);
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
    auto on_empty_or = optional_string_property(**object_or, "filter", "onEmpty", "drop");
    if (!on_empty_or.ok()) {
        return on_empty_or.status();
    }
    EmptyChunkPolicy empty_policy;
    if (*on_empty_or == "drop") {
        empty_policy = EmptyChunkPolicy::Drop;
    } else if (*on_empty_or == "keep") {
        empty_policy = EmptyChunkPolicy::Keep;
    } else {
        return absl::InvalidArgumentError("filter `onEmpty` must be \"drop\" or \"keep\"");
    }

    return transform_rows_preserving_chunks(
        **table_or,
        [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            auto keep_or = ExpressionEvaluator::Invoke(**fn_or, {Value::object(clone_row(row))});
            if (!keep_or.ok()) {
                return keep_or.status();
            }
            if (keep_or->type() != Value::Type::Bool) {
                return absl::InvalidArgumentError("filter `fn` must return a boolean");
            }
            if (!keep_or->as_bool()) {
                return std::shared_ptr<ObjectValue>{};
            }
            return clone_row(row);
        },
        empty_policy);
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

    return transform_rows_preserving_chunks(
        **table_or, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            auto mapped_or = ExpressionEvaluator::Invoke(**fn_or, {Value::object(clone_row(row))});
            if (!mapped_or.ok()) {
                return mapped_or.status();
            }
            if (mapped_or->type() != Value::Type::Object) {
                return absl::InvalidArgumentError("map `fn` must return an object");
            }
            return std::make_shared<ObjectValue>(mapped_or->as_object());
        });
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

    const size_t begin = static_cast<size_t>(offset);
    return slice_table_like(**table_or, [&](size_t size) {
        const size_t end = std::min(size, begin + static_cast<size_t>(*n_or));
        return std::pair<size_t, size_t>{begin, end};
    });
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

    return slice_table_like(**table_or, [&](size_t row_count) {
        const size_t tail_end =
            offset >= static_cast<int64_t>(row_count) ? 0 : row_count - static_cast<size_t>(offset);
        const size_t tail_begin =
            static_cast<size_t>(*n_or) >= tail_end ? 0 : tail_end - static_cast<size_t>(*n_or);
        return std::pair<size_t, size_t>{tail_begin, tail_end};
    });
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
    const std::unordered_set<std::string> selected(columns_or->begin(), columns_or->end());
    return transform_rows_preserving_chunks(
        **table_or, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            std::vector<std::pair<std::string, Value>> props;
            props.reserve(row.properties.size());
            for (const auto& [key, value] : row.properties) {
                if (selected.count(key) != 0) {
                    props.emplace_back(key, value);
                }
            }
            return std::make_shared<ObjectValue>(std::move(props));
        });
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
    const std::unordered_set<std::string> dropped(columns_or->begin(), columns_or->end());
    return transform_rows_preserving_chunks(
        **table_or, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            std::vector<std::pair<std::string, Value>> props;
            props.reserve(row.properties.size());
            for (const auto& [key, value] : row.properties) {
                if (dropped.count(key) == 0) {
                    props.emplace_back(key, value);
                }
            }
            return std::make_shared<ObjectValue>(std::move(props));
        });
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

    return transform_rows_preserving_chunks(
        **table_or, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            std::vector<std::pair<std::string, Value>> props;
            props.reserve(row.properties.size());
            for (const auto& [key, value] : row.properties) {
                if (auto renamed = mapped_column_name(*columns_or, key); renamed.has_value()) {
                    props.emplace_back(*renamed, value);
                } else {
                    props.emplace_back(key, value);
                }
            }
            return std::make_shared<ObjectValue>(std::move(props));
        });
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

    return transform_rows_preserving_chunks(
        **table_or, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            const Value* value = row.lookup(*column_or);
            if (value == nullptr) {
                return clone_row(row);
            }
            auto duplicated = object_with_upserted_property(row, *as_or, *value);
            return std::make_shared<ObjectValue>(duplicated.as_object());
        });
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

    return transform_rows_preserving_chunks(
        **table_or, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            auto updated = object_with_upserted_property(row, *key_or, **value_or);
            return std::make_shared<ObjectValue>(updated.as_object());
        });
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

    auto chunks = clone_table_chunks(**table_or);
    for (auto& chunk : chunks) {
        std::stable_sort(
            chunk.rows.begin(), chunk.rows.end(), [&](const auto& lhs, const auto& rhs) {
                if (lhs == nullptr || rhs == nullptr) {
                    return lhs != nullptr;
                }
                for (const auto& column : *columns_or) {
                    const int cmp = compare_values(lhs->lookup(column), rhs->lookup(column));
                    if (cmp != 0) {
                        return *desc_or ? cmp > 0 : cmp < 0;
                    }
                }
                return false;
            });
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
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
    auto mode_or = optional_string_property(**object_or, "group", "mode", "by");
    if (!mode_or.ok()) {
        return mode_or.status();
    }
    if (*mode_or != "by" && *mode_or != "except") {
        return absl::InvalidArgumentError("group `mode` must be either \"by\" or \"except\"");
    }

    std::vector<std::string> group_columns = *columns_or;
    if (*mode_or == "except") {
        group_columns.clear();
        const std::unordered_set<std::string> excluded(columns_or->begin(), columns_or->end());
        for (const auto& column : all_visible_columns_in_order(**table_or)) {
            if (excluded.count(column) == 0) {
                group_columns.push_back(column);
            }
        }
    }

    std::vector<TableChunk> chunks;
    std::unordered_map<std::string, size_t> chunk_indexes;
    chunks.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr) {
            auto [grouped_row, key] = clone_row_with_group_and_key(*row, group_columns);
            auto [it, inserted] = chunk_indexes.emplace(key, chunks.size());
            if (inserted) {
                chunks.emplace_back();
            }
            chunks[it->second].rows.push_back(std::move(grouped_row));
        }
    }
    if (chunks.empty()) {
        chunks.emplace_back();
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
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
    std::unordered_map<std::string, size_t> row_key_indexes;
    row_key_indexes.reserve(row_key_or->size());
    for (size_t i = 0; i < row_key_or->size(); ++i) {
        row_key_indexes.emplace((*row_key_or)[i], i);
    }
    std::unordered_map<std::string, size_t> column_key_indexes;
    column_key_indexes.reserve(column_key_or->size());
    for (size_t i = 0; i < column_key_or->size(); ++i) {
        column_key_indexes.emplace((*column_key_or)[i], i);
    }

    std::vector<TableChunk> chunks;
    chunks.reserve((*table_or)->table_count());
    for (const auto& chunk : (*table_or)->tables) {
        TableChunk next;
        next.group_key = chunk.group_key;
        std::unordered_map<std::string, size_t> row_indexes;
        std::unordered_map<std::string, std::string> pivot_name_cache;
        std::vector<PivotOutputRow> output_rows;
        row_indexes.reserve(chunk.rows.size());
        pivot_name_cache.reserve(chunk.rows.size());
        output_rows.reserve(chunk.rows.size());
        next.rows.reserve(chunk.rows.size());
        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
            PivotRowProjection projection =
                project_pivot_row(*row, row_key_indexes, column_key_indexes, *value_column_or);
            const std::string identity =
                row_identity_key_from_values(*row_key_or, projection.row_key_values);
            size_t row_index = 0;
            if (const auto existing = row_indexes.find(identity); existing != row_indexes.end()) {
                row_index = existing->second;
            } else {
                std::vector<std::pair<std::string, Value>> props;
                props.reserve(projection.row_key_values.size() +
                              projection.passthrough_props.size() +
                              std::max<size_t>(1, pivot_name_cache.size()));
                PivotOutputRow output_state;
                output_state.property_indexes.reserve(props.capacity());
                for (size_t i = 0; i < row_key_or->size(); ++i) {
                    if (const Value* value = projection.row_key_values[i]; value != nullptr) {
                        props.emplace_back((*row_key_or)[i], *value);
                        output_state.property_indexes.emplace((*row_key_or)[i], props.size() - 1);
                    }
                }
                for (const auto& [key, value] : projection.passthrough_props) {
                    props.emplace_back(key, value);
                    output_state.property_indexes.emplace(key, props.size() - 1);
                }
                auto output_row = std::make_shared<ObjectValue>(std::move(props));
                output_state.row = output_row;
                row_index = output_rows.size();
                next.rows.push_back(output_row);
                output_rows.push_back(std::move(output_state));
                row_indexes.emplace(identity, row_index);
            }

            const Value* value = projection.value;
            if (value == nullptr) {
                continue;
            }
            PivotColumnIdentity column_identity =
                pivot_column_identity_from_values(projection.column_key_values);
            auto [pivot_name_it, inserted] =
                pivot_name_cache.try_emplace(column_identity.cache_key);
            if (inserted) {
                pivot_name_it->second = std::move(column_identity.name);
            }
            upsert_property_with_index(output_rows[row_index], pivot_name_it->second, *value);
        }
        chunks.push_back(std::move(next));
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
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

absl::StatusOr<Value> builtin_union(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "union");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto tables_or = require_table_array_property(**object_or, "union", "tables");
    if (!tables_or.ok()) {
        return tables_or.status();
    }

    std::vector<TableChunk> chunks;
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
        const auto& table_chunks = table->tables;
        chunks.insert(chunks.end(), table_chunks.begin(), table_chunks.end());
    }
    if (chunks.empty()) {
        chunks.emplace_back();
    }
    return Value::table_stream(bucket, std::move(chunks), range_start, range_stop);
}

} // namespace

void InstallUniverseTransformBuiltins(Environment& env) {
    install_builtin(env, "from", builtin_from);
    install_builtin(env, "range", builtin_range, "tables");
    install_builtin(env, "filter", builtin_filter, "tables");
    install_builtin(env, "map", builtin_map, "tables");
    install_builtin(env, "limit", builtin_limit, "tables");
    install_builtin(env, "tail", builtin_tail, "tables");
    install_builtin(env, "keep", builtin_keep, "tables");
    install_builtin(env, "drop", builtin_drop, "tables");
    install_builtin(env, "rename", builtin_rename, "tables");
    install_builtin(env, "duplicate", builtin_duplicate, "tables");
    install_builtin(env, "set", builtin_set, "tables");
    install_builtin(env, "sort", builtin_sort, "tables");
    install_builtin(env, "group", builtin_group, "tables");
    install_builtin(env, "pivot", builtin_pivot, "tables");
    install_builtin(env, "fill", builtin_fill, "tables");
    install_builtin(env, "union", builtin_union);
}

bool InstallKnownUniverseTransformBuiltin(Environment& env, const std::string& name) {
    if (name == "from") {
        install_builtin(env, "from", builtin_from);
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
    if (name == "union") {
        install_builtin(env, "union", builtin_union);
        return true;
    }
    return false;
}

} // namespace pl
