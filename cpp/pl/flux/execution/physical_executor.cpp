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
// Created: 2026/05/10 00:00

#include "cpp/pl/flux/execution/physical_executor.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/connector_registry.h"
#include "cpp/pl/flux/connector/connector_runtime.h"
#include "cpp/pl/flux/optimizer/cbo.h"
#include "cpp/pl/flux/runtime/runtime_builtin_aggregate_helpers.h"
#include "cpp/pl/flux/runtime/runtime_builtin_table_helpers.h"
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pl::flux::execution {
namespace {
using namespace detail;

Value literal_value(const plan::PredicateLiteral& literal) {
    switch (literal.kind) {
        case plan::PredicateLiteralKind::Bool:
            return Value::boolean(literal.bool_value);
        case plan::PredicateLiteralKind::Int:
            return Value::integer(literal.int_value);
        case plan::PredicateLiteralKind::UInt:
            return Value::uinteger(literal.uint_value);
        case plan::PredicateLiteralKind::Float:
            return Value::floating(literal.float_value);
        case plan::PredicateLiteralKind::String:
            return Value::string(literal.string_value);
        case plan::PredicateLiteralKind::Time:
            return Value::time(literal.string_value);
    }
    return Value::null();
}

absl::StatusOr<bool> predicate_matches(const ObjectValue& row,
                                       const plan::PredicateSpec& predicate) {
    const Value* lhs = row.lookup(predicate.column);
    if (lhs == nullptr) {
        return absl::InvalidArgumentError(
            absl::StrCat("memory filter references unavailable column: ", predicate.column));
    }
    const Value rhs = literal_value(predicate.literal);
    const int cmp = compare_values(lhs, &rhs);
    switch (predicate.op) {
        case plan::PredicateOp::Eq:
            return cmp == 0;
        case plan::PredicateOp::NotEq:
            return cmp != 0;
        case plan::PredicateOp::Lt:
            return cmp < 0;
        case plan::PredicateOp::Lte:
            return cmp <= 0;
        case plan::PredicateOp::Gt:
            return cmp > 0;
        case plan::PredicateOp::Gte:
            return cmp >= 0;
    }
    return false;
}

bool time_in_range(const ObjectValue& row, const plan::RangeSpec& range_spec) {
    const Value* value = row.lookup("_time");
    if (value == nullptr) {
        return false;
    }
    const std::string literal =
        value->type() == Value::Type::Time ? value->as_time().literal : value->string();
    if (!range_spec.start.empty() && literal < range_spec.start) {
        return false;
    }
    return !range_spec.stop.has_value() || literal < *range_spec.stop;
}

Value table_with_plan(Value value, const std::shared_ptr<plan::PlanNode>& plan) {
    value.as_table_mut().plan = plan;
    value.as_table_mut().materialized = true;
    return value;
}

connector::SourceSpec source_spec_from_plan(const optimizer::PushdownPlan& plan) {
    return connector::SourceSpec{.source = plan.source->source,
                                 .driver = plan.source->driver,
                                 .dsn = plan.source->dsn,
                                 .table = plan.source->table};
}

connector::SourceSpec source_spec_from_split(const connector::ConnectorSplit& split) {
    return connector::SourceSpec{.source = split.table.source,
                                 .driver = split.table.driver,
                                 .dsn = split.table.dsn,
                                 .table = split.table.table};
}

absl::Status validate_executable_pushdown_plan(const optimizer::PushdownPlan& plan) {
    if (plan.source == nullptr) {
        return absl::InvalidArgumentError("pushdown plan has no source");
    }
    if (!optimizer::CanExecutePushdownPlan(plan)) {
        return absl::InvalidArgumentError("group without aggregate is not executable pushdown");
    }
    return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<connector::ConnectorRuntime>> create_connector_runtime(
    const optimizer::PushdownPlan& plan) {
    auto status = validate_executable_pushdown_plan(plan);
    if (!status.ok()) {
        return status;
    }
    return connector::ConnectorRegistry::Global().CreateRuntime(source_spec_from_plan(plan));
}

absl::StatusOr<std::vector<connector::ConnectorSplit>> create_connector_splits(
    connector::ConnectorRuntime* runtime, const optimizer::PushdownPlan& plan) {
    if (runtime == nullptr) {
        return absl::InvalidArgumentError("connector runtime is missing");
    }
    const connector::SourceSpec spec = source_spec_from_plan(plan);
    auto handle_or = runtime->metadata->GetTableHandle(spec);
    if (!handle_or.ok()) {
        return handle_or.status();
    }
    auto splits_or = runtime->split_manager->GetSplits(*handle_or, plan.request);
    if (!splits_or.ok()) {
        return splits_or.status();
    }
    if (splits_or->empty()) {
        return absl::InvalidArgumentError("connector split manager returned no splits");
    }
    return *splits_or;
}

absl::StatusOr<Value> apply_sort(const Value& input, const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& table = input.as_table();
    auto chunks = clone_table_chunks(table);
    for (auto& chunk : chunks) {
        std::stable_sort(
            chunk.rows.begin(), chunk.rows.end(), [&](const auto& lhs, const auto& rhs) {
                if (lhs == nullptr || rhs == nullptr) {
                    return lhs != nullptr;
                }
                for (const auto& key : plan->sort().keys) {
                    const int cmp =
                        compare_values(lhs->lookup(key.column), rhs->lookup(key.column));
                    if (cmp != 0) {
                        return key.desc ? cmp > 0 : cmp < 0;
                    }
                }
                return false;
            });
    }
    return table_with_plan(table_with_chunks_like(table, std::move(chunks)), plan);
}

absl::StatusOr<Value> apply_group(const Value& input, const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& table = input.as_table();
    std::vector<TableChunk> chunks;
    std::unordered_map<std::string, size_t> chunk_indexes;
    for (const auto& row : table.rows) {
        if (row == nullptr) {
            continue;
        }
        auto [grouped_row, key] = clone_row_with_group_and_key(*row, plan->group().columns);
        auto [it, inserted] = chunk_indexes.emplace(key, chunks.size());
        if (inserted) {
            chunks.emplace_back();
        }
        chunks[it->second].rows.push_back(std::move(grouped_row));
    }
    if (chunks.empty()) {
        chunks.emplace_back();
    }
    return table_with_plan(table_with_chunks_like(table, std::move(chunks)), plan);
}

absl::StatusOr<Value> apply_distinct(const Value& input,
                                     const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& table = input.as_table();
    std::vector<TableChunk> chunks;
    chunks.reserve(table.table_count());
    for (const auto& chunk : table.tables) {
        std::unordered_set<std::string> seen;
        TableChunk next;
        next.group_key = chunk.group_key;
        next.columns = chunk.columns;
        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
            const Value* value = row->lookup(plan->distinct().column);
            const std::string key = value == nullptr ? "<missing>" : value->string();
            if (seen.insert(key).second) {
                next.rows.push_back(row);
            }
        }
        chunks.push_back(std::move(next));
    }
    return table_with_plan(table_with_chunks_like(table, std::move(chunks)), plan);
}

absl::StatusOr<Value> apply_aggregate(const Value& input,
                                      const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& table = input.as_table();
    std::vector<TableChunk> chunks;
    chunks.reserve(table.table_count());
    for (const auto& chunk : table.tables) {
        TableChunk next;
        if (plan->aggregate().fn == plan::AggregateFunction::Count) {
            int64_t count = 0;
            for (const auto& row : chunk.rows) {
                if (row != nullptr && row->lookup(plan->aggregate().column) != nullptr) {
                    ++count;
                }
            }
            next.rows.push_back(
                materialize_group_count_row(chunk, plan->aggregate().column, count));
        } else {
            auto values_or = numeric_values_for_chunk(chunk, "aggregate", plan->aggregate().column);
            if (!values_or.ok()) {
                return values_or.status();
            }
            if (values_or->empty()) {
                continue;
            }
            double value = 0.0;
            switch (plan->aggregate().fn) {
                case plan::AggregateFunction::Sum:
                    for (double item : *values_or) {
                        value += item;
                    }
                    break;
                case plan::AggregateFunction::Mean:
                    for (double item : *values_or) {
                        value += item;
                    }
                    value /= static_cast<double>(values_or->size());
                    break;
                case plan::AggregateFunction::Min:
                    value = *std::min_element(values_or->begin(), values_or->end());
                    break;
                case plan::AggregateFunction::Max:
                    value = *std::max_element(values_or->begin(), values_or->end());
                    break;
                case plan::AggregateFunction::Count:
                    break;
            }
            next.rows.push_back(materialize_group_value_row(chunk, plan->aggregate().column,
                                                            Value::floating(value)));
        }
        chunks.push_back(std::move(next));
    }
    return table_with_plan(table_with_chunks_like(table, std::move(chunks)), plan);
}

Value value_from_page(const Page& page) {
    TableValue table = TableValueFromPage(page);
    Value value = Value::table_stream(table.bucket, std::move(table.tables), table.range_start,
                                      table.range_stop, table.result_name);
    value.as_table_mut().plan = table.plan;
    value.as_table_mut().materialized = table.materialized;
    return value;
}

Page page_from_value(const Value& value) { return PageFromTableValue(value.as_table()); }

Page page_with_plan(Page page, const std::shared_ptr<plan::PlanNode>& plan) {
    page.plan = plan;
    return page;
}

absl::StatusOr<Page> apply_range_page(Page input, const std::shared_ptr<plan::PlanNode>& plan) {
    auto status = ValidatePage(input);
    if (!status.ok()) {
        return status;
    }
    std::vector<PageChunk> chunks;
    chunks.reserve(input.chunks.size());
    for (const auto& source : input.chunks) {
        PageChunk chunk;
        chunk.group_key = source.group_key;
        chunk.row_count = 0;
        chunk.columns.reserve(source.columns.size());
        for (const auto& source_column : source.columns) {
            chunk.columns.push_back(
                ColumnVector{.name = source_column.name, .type = source_column.type});
        }
        for (size_t row_index = 0; row_index < source.row_count; ++row_index) {
            auto row = RowFromPageChunk(source, row_index);
            if (!time_in_range(*row, plan->range())) {
                continue;
            }
            for (size_t column_index = 0; column_index < source.columns.size(); ++column_index) {
                chunk.columns[column_index].values.push_back(
                    source.columns[column_index].values[row_index]);
            }
            ++chunk.row_count;
        }
        chunks.push_back(std::move(chunk));
    }
    return page_with_plan(PageLike(input, std::move(chunks)), plan);
}

absl::StatusOr<Page> apply_filter_page(Page input, const std::shared_ptr<plan::PlanNode>& plan) {
    auto status = ValidatePage(input);
    if (!status.ok()) {
        return status;
    }
    std::vector<PageChunk> chunks;
    chunks.reserve(input.chunks.size());
    for (const auto& source : input.chunks) {
        PageChunk chunk;
        chunk.group_key = source.group_key;
        chunk.row_count = 0;
        chunk.columns.reserve(source.columns.size());
        for (const auto& source_column : source.columns) {
            chunk.columns.push_back(
                ColumnVector{.name = source_column.name, .type = source_column.type});
        }
        for (size_t row_index = 0; row_index < source.row_count; ++row_index) {
            auto row = RowFromPageChunk(source, row_index);
            bool keep = true;
            for (const auto& predicate : plan->filter().predicates) {
                auto matches_or = predicate_matches(*row, predicate);
                if (!matches_or.ok()) {
                    return matches_or.status();
                }
                if (!*matches_or) {
                    keep = false;
                    break;
                }
            }
            if (!keep) {
                continue;
            }
            for (size_t column_index = 0; column_index < source.columns.size(); ++column_index) {
                chunk.columns[column_index].values.push_back(
                    source.columns[column_index].values[row_index]);
            }
            ++chunk.row_count;
        }
        chunks.push_back(std::move(chunk));
    }
    return page_with_plan(PageLike(input, std::move(chunks)), plan);
}

absl::StatusOr<Page> apply_project_page(Page input, const std::shared_ptr<plan::PlanNode>& plan) {
    auto status = ValidatePage(input);
    if (!status.ok()) {
        return status;
    }
    std::vector<PageChunk> chunks;
    chunks.reserve(input.chunks.size());
    for (const auto& source : input.chunks) {
        PageChunk chunk;
        chunk.group_key = source.group_key;
        chunk.row_count = source.row_count;
        chunk.columns.reserve(plan->project().columns.size());
        PageSchema schema = SchemaFromPageChunk(source);
        for (const auto& column_name : plan->project().columns) {
            ColumnVector column;
            column.name = column_name;
            column.values.reserve(source.row_count);
            auto source_index = schema.FindColumn(column_name);
            if (source_index.has_value()) {
                const auto& source_column = source.columns[*source_index];
                column.type = source_column.type;
                column.values = source_column.values;
            } else {
                column.type = Value::Type::Null;
                column.values.assign(source.row_count, Value::null());
            }
            chunk.columns.push_back(std::move(column));
        }
        chunks.push_back(std::move(chunk));
    }
    return page_with_plan(PageLike(input, std::move(chunks)), plan);
}

absl::StatusOr<Page> apply_rename_page(Page input, const std::shared_ptr<plan::PlanNode>& plan) {
    auto status = ValidatePage(input);
    if (!status.ok()) {
        return status;
    }
    for (auto& chunk : input.chunks) {
        for (auto& column : chunk.columns) {
            column.name =
                mapped_column_name(plan->rename().columns, column.name).value_or(column.name);
        }
    }
    return page_with_plan(std::move(input), plan);
}

Page materialize_page(Page input, const std::shared_ptr<plan::PlanNode>& plan) {
    input.plan = plan;
    input.materialized = true;
    return input;
}

std::optional<std::string> exchange_partition_key(const ObjectValue& row,
                                                  const std::vector<std::string>& columns) {
    if (columns.empty()) {
        return std::string("__single_partition__");
    }
    std::string key;
    for (const auto& column : columns) {
        const Value* value = row.lookup(column);
        if (value == nullptr || value->is_null()) {
            absl::StrAppend(&key, column, "=<null>\n");
        } else {
            absl::StrAppend(&key, column, "=", value->string(), "\n");
        }
    }
    return key;
}

Value apply_exchange_value(const Value& input_value, const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& input = input_value.as_table();
    std::vector<TableChunk> chunks;
    if (plan->exchange().kind == plan::ExchangeKind::Gather ||
        plan->exchange().partition_keys.empty()) {
        TableChunk chunk;
        chunk.rows = input.rows;
        chunk.columns = all_visible_columns_in_order(input);
        chunks.push_back(std::move(chunk));
    } else {
        std::unordered_map<std::string, size_t> partition_index;
        std::vector<std::string> partition_order;
        for (const auto& row : input.rows) {
            if (row == nullptr) {
                continue;
            }
            auto key = exchange_partition_key(*row, plan->exchange().partition_keys);
            if (!key.has_value()) {
                continue;
            }
            auto it = partition_index.find(*key);
            if (it == partition_index.end()) {
                const size_t index = chunks.size();
                partition_index.emplace(*key, index);
                partition_order.push_back(*key);
                chunks.push_back(TableChunk{});
                it = partition_index.find(partition_order.back());
            }
            chunks[it->second].rows.push_back(row);
        }
        const auto columns = all_visible_columns_in_order(input);
        for (auto& chunk : chunks) {
            chunk.columns = columns;
        }
    }
    Value value = Value::table_stream(input.bucket, std::move(chunks), input.range_start,
                                      input.range_stop, input.result_name);
    value.as_table_mut().plan = plan;
    value.as_table_mut().materialized = true;
    return value;
}

std::optional<std::string> local_join_key(const ObjectValue& row,
                                          const std::vector<std::string>& columns) {
    std::string key;
    for (const auto& column : columns) {
        const Value* value = row.lookup(column);
        if (value == nullptr || value->is_null()) {
            return std::nullopt;
        }
        absl::StrAppend(&key, column, "=", value->string(), "\n");
    }
    return key;
}

std::shared_ptr<ObjectValue> null_row_for_columns(const std::vector<std::string>& columns) {
    std::vector<std::pair<std::string, Value>> props;
    props.reserve(columns.size());
    for (const auto& column : columns) {
        props.emplace_back(column, Value::null());
    }
    return std::make_shared<ObjectValue>(std::move(props));
}

std::shared_ptr<ObjectValue> local_join_row(const ObjectValue* left,
                                            const std::vector<std::string>& left_columns,
                                            const ObjectValue* right,
                                            const std::vector<std::string>& right_columns,
                                            const plan::JoinSpec& spec) {
    const std::unordered_set<std::string> on_columns(spec.on.begin(), spec.on.end());
    const std::unordered_set<std::string> right_column_set(right_columns.begin(),
                                                           right_columns.end());
    std::vector<std::pair<std::string, Value>> props;
    props.reserve(left_columns.size() + right_columns.size());

    auto append = [&](const std::string& column, const Value* value) {
        props.emplace_back(column, value == nullptr ? Value::null() : *value);
    };
    for (const auto& column : spec.on) {
        const Value* value = left == nullptr ? nullptr : left->lookup(column);
        if ((value == nullptr || value->is_null()) && right != nullptr) {
            value = right->lookup(column);
        }
        append(column, value);
    }
    for (const auto& column : left_columns) {
        if (column == "_group" || on_columns.count(column) != 0) {
            continue;
        }
        const std::string output_name = right_column_set.count(column) == 0
                                            ? column
                                            : absl::StrCat(column, "_", spec.left_name);
        append(output_name, left == nullptr ? nullptr : left->lookup(column));
    }
    const std::unordered_set<std::string> left_column_set(left_columns.begin(), left_columns.end());
    for (const auto& column : right_columns) {
        if (column == "_group" || on_columns.count(column) != 0) {
            continue;
        }
        const std::string output_name = left_column_set.count(column) == 0
                                            ? column
                                            : absl::StrCat(column, "_", spec.right_name);
        append(output_name, right == nullptr ? nullptr : right->lookup(column));
    }
    return std::make_shared<ObjectValue>(std::move(props));
}

absl::StatusOr<Value> local_hash_join_values(const Value& left_value,
                                             const Value& right_value,
                                             const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& spec = plan->join();
    if (spec.on.empty()) {
        return absl::InvalidArgumentError("local hash join requires at least one key");
    }
    const TableValue& left_table = left_value.as_table();
    const TableValue& right_table = right_value.as_table();
    const auto left_columns = all_visible_columns_in_order(left_table);
    const auto right_columns = all_visible_columns_in_order(right_table);
    auto left_null = null_row_for_columns(left_columns);
    auto right_null = null_row_for_columns(right_columns);

    TableChunk chunk;
    if (spec.build_side == plan::JoinBuildSide::Left) {
        std::unordered_map<std::string, std::vector<size_t>> left_rows_by_key;
        left_rows_by_key.reserve(left_table.rows.size());
        for (size_t row_index = 0; row_index < left_table.rows.size(); ++row_index) {
            const auto& row = left_table.rows[row_index];
            if (row == nullptr) {
                continue;
            }
            auto key = local_join_key(*row, spec.on);
            if (key.has_value()) {
                left_rows_by_key[*key].push_back(row_index);
            }
        }

        std::vector<bool> matched_left(left_table.rows.size(), false);
        for (const auto& right_row : right_table.rows) {
            if (right_row == nullptr) {
                continue;
            }
            bool matched = false;
            auto key = local_join_key(*right_row, spec.on);
            if (key.has_value()) {
                auto it = left_rows_by_key.find(*key);
                if (it != left_rows_by_key.end()) {
                    matched = true;
                    for (size_t left_index : it->second) {
                        matched_left[left_index] = true;
                        chunk.rows.push_back(local_join_row(left_table.rows[left_index].get(),
                                                            left_columns, right_row.get(),
                                                            right_columns, spec));
                    }
                }
            }
            if (!matched &&
                (spec.method == plan::JoinMethod::Right || spec.method == plan::JoinMethod::Full)) {
                chunk.rows.push_back(local_join_row(left_null.get(), left_columns, right_row.get(),
                                                    right_columns, spec));
            }
        }
        if (spec.method == plan::JoinMethod::Left || spec.method == plan::JoinMethod::Full) {
            for (size_t left_index = 0; left_index < left_table.rows.size(); ++left_index) {
                if (matched_left[left_index] || left_table.rows[left_index] == nullptr) {
                    continue;
                }
                chunk.rows.push_back(local_join_row(left_table.rows[left_index].get(), left_columns,
                                                    right_null.get(), right_columns, spec));
            }
        }
    } else {
        std::unordered_map<std::string, std::vector<size_t>> right_rows_by_key;
        right_rows_by_key.reserve(right_table.rows.size());
        for (size_t row_index = 0; row_index < right_table.rows.size(); ++row_index) {
            const auto& row = right_table.rows[row_index];
            if (row == nullptr) {
                continue;
            }
            auto key = local_join_key(*row, spec.on);
            if (key.has_value()) {
                right_rows_by_key[*key].push_back(row_index);
            }
        }

        std::vector<bool> matched_right(right_table.rows.size(), false);
        for (const auto& left_row : left_table.rows) {
            if (left_row == nullptr) {
                continue;
            }
            bool matched = false;
            auto key = local_join_key(*left_row, spec.on);
            if (key.has_value()) {
                auto it = right_rows_by_key.find(*key);
                if (it != right_rows_by_key.end()) {
                    matched = true;
                    for (size_t right_index : it->second) {
                        matched_right[right_index] = true;
                        chunk.rows.push_back(local_join_row(left_row.get(), left_columns,
                                                            right_table.rows[right_index].get(),
                                                            right_columns, spec));
                    }
                }
            }
            if (!matched &&
                (spec.method == plan::JoinMethod::Left || spec.method == plan::JoinMethod::Full)) {
                chunk.rows.push_back(local_join_row(left_row.get(), left_columns, right_null.get(),
                                                    right_columns, spec));
            }
        }
        if (spec.method == plan::JoinMethod::Right || spec.method == plan::JoinMethod::Full) {
            for (size_t right_index = 0; right_index < right_table.rows.size(); ++right_index) {
                if (matched_right[right_index] || right_table.rows[right_index] == nullptr) {
                    continue;
                }
                chunk.rows.push_back(local_join_row(left_null.get(), left_columns,
                                                    right_table.rows[right_index].get(),
                                                    right_columns, spec));
            }
        }
    }

    std::vector<TableChunk> chunks;
    chunks.push_back(std::move(chunk));
    Value value = Value::table_stream(
        left_table.bucket.empty() ? right_table.bucket : left_table.bucket, std::move(chunks),
        left_table.range_start.has_value() ? left_table.range_start : right_table.range_start,
        left_table.range_stop.has_value() ? left_table.range_stop : right_table.range_stop);
    value.as_table_mut().plan = plan;
    value.as_table_mut().materialized = true;
    return value;
}

absl::StatusOr<std::optional<Page>> next_input_page(Operator* input) {
    if (input == nullptr) {
        return absl::InvalidArgumentError("operator has no input");
    }
    return input->NextPage();
}

absl::StatusOr<Value> collect_input_value(Operator* input) {
    if (input == nullptr) {
        return absl::InvalidArgumentError("operator has no input");
    }
    std::optional<Page> output;
    while (true) {
        auto page_or = input->NextPage();
        if (!page_or.ok()) {
            return page_or.status();
        }
        if (!page_or->has_value()) {
            break;
        }
        if (!output.has_value()) {
            output = std::move(page_or->value());
            continue;
        }
        AppendPage(&*output, std::move(page_or->value()));
    }
    if (!output.has_value()) {
        return Value::table_stream("", {});
    }
    return value_from_page(*output);
}

class ConnectorScanOperator final : public Operator {
public:
    explicit ConnectorScanOperator(optimizer::PushdownPlan plan) : plan_(std::move(plan)) {}

    [[nodiscard]] std::string name() const override { return "ConnectorScanOperator"; }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (!initialized_) {
            auto status = Initialize();
            if (!status.ok()) {
                return status;
            }
        }

        while (true) {
            if (page_source_ == nullptr) {
                if (next_split_ >= splits_.size()) {
                    return std::nullopt;
                }
                auto page_source_or =
                    runtime_->page_source_provider->CreatePageSource(splits_[next_split_]);
                if (!page_source_or.ok()) {
                    return page_source_or.status();
                }
                page_source_ = std::move(*page_source_or);
                ++next_split_;
            }

            auto page_or = page_source_->NextPage();
            if (!page_or.ok()) {
                return page_or.status();
            }
            if (!page_or->has_value()) {
                if (next_split_ > 0) {
                    splits_[next_split_ - 1].finished = page_source_->Finished();
                    split_stats_.push_back(page_source_->Stats());
                }
                page_source_.reset();
                continue;
            }
            auto status = ValidatePage(page_or->value());
            if (!status.ok()) {
                return status;
            }
            return page_or;
        }
    }

private:
    absl::Status Initialize() {
        auto runtime_or = create_connector_runtime(plan_);
        if (!runtime_or.ok()) {
            return runtime_or.status();
        }
        runtime_ = std::move(*runtime_or);
        auto splits_or = create_connector_splits(runtime_.get(), plan_);
        if (!splits_or.ok()) {
            return splits_or.status();
        }
        splits_ = std::move(*splits_or);
        initialized_ = true;
        return absl::OkStatus();
    }

    optimizer::PushdownPlan plan_;
    std::unique_ptr<connector::ConnectorRuntime> runtime_;
    std::vector<connector::ConnectorSplit> splits_;
    std::vector<connector::ConnectorSplitStats> split_stats_;
    std::unique_ptr<connector::ConnectorPageSource> page_source_;
    size_t next_split_ = 0;
    bool initialized_ = false;
};

class ConnectorSplitScanOperator final : public Operator {
public:
    explicit ConnectorSplitScanOperator(connector::ConnectorSplit split)
        : split_(std::move(split)) {}

    [[nodiscard]] std::string name() const override { return "ConnectorScanOperator"; }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (!initialized_) {
            auto runtime_or = connector::ConnectorRegistry::Global().CreateRuntime(
                source_spec_from_split(split_));
            if (!runtime_or.ok()) {
                return runtime_or.status();
            }
            runtime_ = std::move(*runtime_or);
            auto page_source_or = runtime_->page_source_provider->CreatePageSource(split_);
            if (!page_source_or.ok()) {
                return page_source_or.status();
            }
            page_source_ = std::move(*page_source_or);
            initialized_ = true;
        }
        auto page_or = page_source_->NextPage();
        if (!page_or.ok()) {
            return page_or.status();
        }
        if (!page_or->has_value()) {
            split_.finished = page_source_->Finished();
            split_stats_ = page_source_->Stats();
            return std::nullopt;
        }
        auto status = ValidatePage(page_or->value());
        if (!status.ok()) {
            return status;
        }
        return page_or;
    }

private:
    connector::ConnectorSplit split_;
    connector::ConnectorSplitStats split_stats_;
    std::unique_ptr<connector::ConnectorRuntime> runtime_;
    std::unique_ptr<connector::ConnectorPageSource> page_source_;
    bool initialized_ = false;
};

class ExchangeBuffer {
public:
    explicit ExchangeBuffer(size_t max_pages = 1024) : max_pages_(std::max<size_t>(1, max_pages)) {}

    void SetProducerCount(size_t producer_count) {
        std::lock_guard<std::mutex> lock(mu_);
        producer_count_ = std::max<size_t>(1, producer_count);
    }

    absl::Status AddPage(Page page) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() {
            return closed_ || finished_ || error_.has_value() || pages_.size() < max_pages_;
        });
        if (closed_) {
            return absl::FailedPreconditionError("exchange buffer is closed");
        }
        if (finished_) {
            return absl::FailedPreconditionError("exchange buffer is finished");
        }
        if (error_.has_value()) {
            return absl::FailedPreconditionError(*error_);
        }
        rows_ += page.row_count();
        pages_.push_back(std::move(page));
        lock.unlock();
        cv_.notify_all();
        return absl::OkStatus();
    }

    absl::StatusOr<std::optional<Page>> PopPage() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() {
            return !pages_.empty() || finished_ || error_.has_value();
        });
        if (error_.has_value()) {
            return absl::FailedPreconditionError(*error_);
        }
        if (pages_.empty()) {
            return std::nullopt;
        }
        Page page = std::move(pages_.front());
        pages_.pop_front();
        lock.unlock();
        cv_.notify_all();
        return page;
    }

    [[nodiscard]] size_t page_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return pages_.size();
    }
    [[nodiscard]] size_t row_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return rows_;
    }
    [[nodiscard]] bool finished() const {
        std::lock_guard<std::mutex> lock(mu_);
        return finished_;
    }
    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mu_);
        return closed_;
    }

    absl::Status Finish() {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) {
            return absl::FailedPreconditionError("exchange buffer is closed");
        }
        if (error_.has_value()) {
            return absl::FailedPreconditionError(*error_);
        }
        ++finished_producers_;
        if (finished_producers_ >= producer_count_) {
            finished_ = true;
        }
        cv_.notify_all();
        return absl::OkStatus();
    }

    void MarkError(const absl::Status& status) {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = status.ToString();
        finished_ = true;
        cv_.notify_all();
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        finished_ = true;
        cv_.notify_all();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Page> pages_;
    size_t max_pages_ = 1024;
    size_t producer_count_ = 1;
    size_t finished_producers_ = 0;
    size_t rows_ = 0;
    bool finished_ = false;
    bool closed_ = false;
    std::optional<std::string> error_;
};

class ExchangeSinkOperator final : public Operator {
public:
    ExchangeSinkOperator(std::unique_ptr<Operator> input, std::shared_ptr<ExchangeBuffer> buffer)
        : input_(std::move(input)), buffer_(std::move(buffer)) {}

    [[nodiscard]] std::string name() const override { return "ExchangeSinkOperator"; }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (finished_) {
            return std::nullopt;
        }
        if (buffer_ == nullptr) {
            return absl::InvalidArgumentError("exchange sink has no buffer");
        }
        auto page_or = next_input_page(input_.get());
        if (!page_or.ok()) {
            buffer_->MarkError(page_or.status());
            return page_or.status();
        }
        if (!page_or->has_value()) {
            auto status = buffer_->Finish();
            if (!status.ok()) {
                return status;
            }
            finished_ = true;
            return std::nullopt;
        }
        if (buffer_->closed()) {
            return absl::FailedPreconditionError("exchange sink writes to closed buffer");
        }
        Page output = std::move(**page_or);
        Page buffered = output;
        auto status = buffer_->AddPage(std::move(buffered));
        if (!status.ok()) {
            return status;
        }
        return output;
    }

private:
    std::unique_ptr<Operator> input_;
    std::shared_ptr<ExchangeBuffer> buffer_;
    bool finished_ = false;
};

class ExchangeSourceOperator final : public Operator {
public:
    explicit ExchangeSourceOperator(std::shared_ptr<ExchangeBuffer> buffer)
        : buffer_(std::move(buffer)) {}

    [[nodiscard]] std::string name() const override { return "ExchangeSourceOperator"; }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (buffer_ == nullptr) {
            return absl::InvalidArgumentError("exchange source has no buffer");
        }
        return buffer_->PopPage();
    }

private:
    std::shared_ptr<ExchangeBuffer> buffer_;
};

class PageUnaryOperator : public Operator {
public:
    PageUnaryOperator(std::shared_ptr<plan::PlanNode> plan, std::unique_ptr<Operator> input)
        : plan_(std::move(plan)), input_(std::move(input)) {}

    absl::StatusOr<std::optional<Page>> NextPage() override {
        auto input_or = next_input_page(input_.get());
        if (!input_or.ok()) {
            return input_or.status();
        }
        if (!input_or->has_value()) {
            return std::nullopt;
        }
        auto page_or = Apply(std::move(**input_or));
        if (!page_or.ok()) {
            return page_or.status();
        }
        return std::move(*page_or);
    }

protected:
    [[nodiscard]] const std::shared_ptr<plan::PlanNode>& plan() const { return plan_; }
    [[nodiscard]] Operator* input() const { return input_.get(); }
    virtual absl::StatusOr<Page> Apply(Page input) const = 0;

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
};

class BlockingUnaryOperator : public Operator {
public:
    BlockingUnaryOperator(std::shared_ptr<plan::PlanNode> plan, std::unique_ptr<Operator> input)
        : plan_(std::move(plan)), input_(std::move(input)) {}

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (emitted_) {
            return std::nullopt;
        }
        emitted_ = true;
        auto input_or = collect_input_value(input_.get());
        if (!input_or.ok()) {
            return input_or.status();
        }
        auto value_or = Apply(*input_or);
        if (!value_or.ok()) {
            return value_or.status();
        }
        return page_from_value(*value_or);
    }

protected:
    [[nodiscard]] const std::shared_ptr<plan::PlanNode>& plan() const { return plan_; }
    virtual absl::StatusOr<Value> Apply(const Value& input) const = 0;

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
    bool emitted_ = false;
};

class RangeOperator final : public PageUnaryOperator {
public:
    using PageUnaryOperator::PageUnaryOperator;
    [[nodiscard]] std::string name() const override { return "RangeOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override {
        return apply_range_page(std::move(input), plan());
    }
};

class FilterOperator final : public PageUnaryOperator {
public:
    using PageUnaryOperator::PageUnaryOperator;
    [[nodiscard]] std::string name() const override { return "FilterOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override {
        return apply_filter_page(std::move(input), plan());
    }
};

class ProjectOperator final : public PageUnaryOperator {
public:
    using PageUnaryOperator::PageUnaryOperator;
    [[nodiscard]] std::string name() const override { return "ProjectOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override {
        return apply_project_page(std::move(input), plan());
    }
};

class RenameOperator final : public PageUnaryOperator {
public:
    using PageUnaryOperator::PageUnaryOperator;
    [[nodiscard]] std::string name() const override { return "RenameOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override {
        return apply_rename_page(std::move(input), plan());
    }
};

class ExchangeOperator final : public Operator {
public:
    ExchangeOperator(std::shared_ptr<plan::PlanNode> plan, std::unique_ptr<Operator> input)
        : plan_(std::move(plan)), input_(std::move(input)) {}

    [[nodiscard]] std::string name() const override { return "ExchangeOperator"; }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (emitted_) {
            return std::nullopt;
        }
        emitted_ = true;
        auto input_or = collect_input_value(input_.get());
        if (!input_or.ok()) {
            return input_or.status();
        }
        return page_from_value(apply_exchange_value(*input_or, plan_));
    }

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
    bool emitted_ = false;
};

class LimitOperator final : public Operator {
public:
    LimitOperator(std::shared_ptr<plan::PlanNode> plan, std::unique_ptr<Operator> input)
        : plan_(std::move(plan)),
          input_(std::move(input)),
          remaining_offset_(std::max<int64_t>(0, plan_->limit().offset)),
          remaining_limit_(std::max<int64_t>(0, plan_->limit().n)) {}
    [[nodiscard]] std::string name() const override { return "LimitOperator"; }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (remaining_limit_ == 0) {
            return std::nullopt;
        }
        while (true) {
            auto input_or = next_input_page(input_.get());
            if (!input_or.ok()) {
                return input_or.status();
            }
            if (!input_or->has_value()) {
                return std::nullopt;
            }

            Page input = std::move(input_or->value());
            auto status = ValidatePage(input);
            if (!status.ok()) {
                return status;
            }
            std::vector<PageChunk> chunks;
            chunks.reserve(input.chunks.size());
            for (const auto& source : input.chunks) {
                if (remaining_limit_ == 0) {
                    break;
                }
                if (remaining_offset_ >= static_cast<int64_t>(source.row_count)) {
                    remaining_offset_ -= static_cast<int64_t>(source.row_count);
                    continue;
                }
                const size_t start = static_cast<size_t>(remaining_offset_);
                remaining_offset_ = 0;
                const size_t take = std::min<size_t>(source.row_count - start,
                                                     static_cast<size_t>(remaining_limit_));
                PageChunk chunk = SlicePageChunkRows(source, start, take);
                remaining_limit_ -= static_cast<int64_t>(chunk.row_count);
                chunks.push_back(std::move(chunk));
            }
            Page output = PageLike(input, std::move(chunks));
            output.plan = plan_;
            if (!output.empty() || remaining_limit_ == 0) {
                return output;
            }
        }
    }

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
    int64_t remaining_offset_ = 0;
    int64_t remaining_limit_ = 0;
};

class SortOperator final : public BlockingUnaryOperator {
public:
    using BlockingUnaryOperator::BlockingUnaryOperator;
    [[nodiscard]] std::string name() const override { return "SortOperator"; }

private:
    absl::StatusOr<Value> Apply(const Value& input) const override {
        return apply_sort(input, plan());
    }
};

class GroupOperator final : public BlockingUnaryOperator {
public:
    using BlockingUnaryOperator::BlockingUnaryOperator;
    [[nodiscard]] std::string name() const override { return "GroupOperator"; }

private:
    absl::StatusOr<Value> Apply(const Value& input) const override {
        return apply_group(input, plan());
    }
};

class DistinctOperator final : public BlockingUnaryOperator {
public:
    using BlockingUnaryOperator::BlockingUnaryOperator;
    [[nodiscard]] std::string name() const override { return "DistinctOperator"; }

private:
    absl::StatusOr<Value> Apply(const Value& input) const override {
        return apply_distinct(input, plan());
    }
};

class AggregateOperator final : public BlockingUnaryOperator {
public:
    using BlockingUnaryOperator::BlockingUnaryOperator;
    [[nodiscard]] std::string name() const override { return "AggregateOperator"; }

private:
    absl::StatusOr<Value> Apply(const Value& input) const override {
        return apply_aggregate(input, plan());
    }
};

class LocalHashJoinOperator final : public Operator {
public:
    LocalHashJoinOperator(std::shared_ptr<plan::PlanNode> plan,
                          std::unique_ptr<Operator> left,
                          std::unique_ptr<Operator> right)
        : plan_(std::move(plan)), left_(std::move(left)), right_(std::move(right)) {}

    [[nodiscard]] std::string name() const override { return "LocalHashJoinOperator"; }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (emitted_) {
            return std::nullopt;
        }
        emitted_ = true;
        auto left_or = collect_input_value(left_.get());
        if (!left_or.ok()) {
            return left_or.status();
        }
        auto right_or = collect_input_value(right_.get());
        if (!right_or.ok()) {
            return right_or.status();
        }
        auto value_or = local_hash_join_values(*left_or, *right_or, plan_);
        if (!value_or.ok()) {
            return value_or.status();
        }
        return page_from_value(*value_or);
    }

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> left_;
    std::unique_ptr<Operator> right_;
    bool emitted_ = false;
};

class MaterializeOperator final : public PageUnaryOperator {
public:
    using PageUnaryOperator::PageUnaryOperator;
    [[nodiscard]] std::string name() const override { return "MaterializeOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override {
        return materialize_page(std::move(input), plan());
    }
};

class OutputOperator final : public Operator {
public:
    explicit OutputOperator(std::unique_ptr<Operator> input) : input_(std::move(input)) {}

    [[nodiscard]] std::string name() const override { return "OutputOperator"; }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        auto input_or = next_input_page(input_.get());
        if (!input_or.ok()) {
            return input_or.status();
        }
        if (!input_or->has_value()) {
            return std::nullopt;
        }
        auto status = ValidatePage(input_or->value());
        if (!status.ok()) {
            return status;
        }
        input_or->value().materialized = true;
        return input_or;
    }

private:
    std::unique_ptr<Operator> input_;
};

struct PlannedOperator {
    std::unique_ptr<Operator> root;
    std::vector<std::unique_ptr<Operator>> driver_roots;
    std::vector<std::string> operators;
};

absl::StatusOr<PlannedOperator> BuildOperator(const std::shared_ptr<plan::PlanNode>& logical_plan,
                                              bool allow_multi_driver = true) {
    if (logical_plan == nullptr) {
        return absl::InvalidArgumentError("missing physical plan root");
    }
    auto cbo_or = optimizer::FastCostBasedOptimizer().OptimizeWithTrace(logical_plan);
    if (!cbo_or.ok()) {
        return cbo_or.status();
    }
    auto& optimized = cbo_or->rbo_result;
    if (optimized.pushdown_plan.has_value() &&
        optimizer::CanExecutePushdownPlan(*optimized.pushdown_plan)) {
        PlannedOperator planned;
        if (allow_multi_driver) {
            auto runtime_or = create_connector_runtime(*optimized.pushdown_plan);
            if (!runtime_or.ok()) {
                return runtime_or.status();
            }
            auto splits_or = create_connector_splits(runtime_or->get(), *optimized.pushdown_plan);
            if (!splits_or.ok()) {
                return splits_or.status();
            }
            if (splits_or->size() > 1) {
                planned.driver_roots.reserve(splits_or->size());
                for (auto& split : *splits_or) {
                    planned.driver_roots.push_back(std::unique_ptr<Operator>(
                        new ConnectorSplitScanOperator(std::move(split))));
                }
                planned.operators.push_back("ConnectorScanOperator");
                return planned;
            }
        }
        planned.root = std::unique_ptr<Operator>(
            new ConnectorScanOperator(std::move(*optimized.pushdown_plan)));
        planned.operators.push_back(planned.root->name());
        return planned;
    }
    const auto& optimized_plan = optimized.plan;
    if (optimized_plan->kind == plan::PlanNodeKind::Join) {
        if (optimized_plan->inputs.size() != 2 || optimized_plan->inputs[0] == nullptr ||
            optimized_plan->inputs[1] == nullptr) {
            return absl::InvalidArgumentError("join plan requires two inputs");
        }
        auto left_or = BuildOperator(optimized_plan->inputs[0], false);
        if (!left_or.ok()) {
            return left_or.status();
        }
        auto right_or = BuildOperator(optimized_plan->inputs[1], false);
        if (!right_or.ok()) {
            return right_or.status();
        }
        PlannedOperator planned;
        planned.operators = std::move(left_or->operators);
        planned.operators.insert(planned.operators.end(), right_or->operators.begin(),
                                 right_or->operators.end());
        planned.root = std::unique_ptr<Operator>(new LocalHashJoinOperator(
            optimized_plan, std::move(left_or->root), std::move(right_or->root)));
        planned.operators.push_back(planned.root->name());
        return planned;
    }
    if (optimized_plan->inputs.size() != 1 || optimized_plan->inputs[0] == nullptr) {
        return absl::InvalidArgumentError("plan is not executable");
    }
    auto planned_or = BuildOperator(optimized_plan->inputs[0], false);
    if (!planned_or.ok()) {
        return planned_or.status();
    }
    std::unique_ptr<Operator> current;
    if (optimized_plan->kind == plan::PlanNodeKind::Materialize) {
        current = std::unique_ptr<Operator>(
            new MaterializeOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Range) {
        current = std::unique_ptr<Operator>(
            new RangeOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Filter) {
        current = std::unique_ptr<Operator>(
            new FilterOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Project) {
        current = std::unique_ptr<Operator>(
            new ProjectOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Rename) {
        current = std::unique_ptr<Operator>(
            new RenameOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Exchange) {
        current = std::unique_ptr<Operator>(
            new ExchangeOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Limit) {
        current = std::unique_ptr<Operator>(
            new LimitOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Sort) {
        current = std::unique_ptr<Operator>(
            new SortOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Group) {
        current = std::unique_ptr<Operator>(
            new GroupOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Distinct) {
        current = std::unique_ptr<Operator>(
            new DistinctOperator(optimized_plan, std::move(planned_or->root)));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Aggregate) {
        current = std::unique_ptr<Operator>(
            new AggregateOperator(optimized_plan, std::move(planned_or->root)));
    } else {
        return absl::InvalidArgumentError(absl::StrCat(
            "unsupported physical operator: ", plan::PlanNodeKindName(optimized_plan->kind)));
    }
    planned_or->root = std::move(current);
    planned_or->operators.push_back(planned_or->root->name());
    return std::move(*planned_or);
}

Pipeline MakePipeline(std::string id,
                      std::string name,
                      std::string role,
                      std::vector<std::string> dependencies,
                      std::vector<std::string> operators,
                      std::unique_ptr<Operator> root,
                      std::vector<std::unique_ptr<Operator>> driver_roots = {}) {
    Pipeline pipeline;
    pipeline.id = std::move(id);
    pipeline.name = std::move(name);
    pipeline.role = std::move(role);
    pipeline.dependencies = std::move(dependencies);
    pipeline.operators = std::move(operators);
    pipeline.root = std::move(root);
    pipeline.driver_roots = std::move(driver_roots);
    return pipeline;
}

std::vector<std::unique_ptr<Operator>> WrapDriverRootsWithExchangeSinks(
    std::vector<std::unique_ptr<Operator>> roots,
    const std::shared_ptr<ExchangeBuffer>& output_buffer) {
    std::vector<std::unique_ptr<Operator>> sinks;
    sinks.reserve(roots.size());
    for (auto& root : roots) {
        sinks.push_back(
            std::unique_ptr<Operator>(new ExchangeSinkOperator(std::move(root), output_buffer)));
    }
    return sinks;
}

std::vector<std::unique_ptr<Operator>> WrapDriverRootsWithOutput(
    std::vector<std::unique_ptr<Operator>> roots) {
    std::vector<std::unique_ptr<Operator>> outputs;
    outputs.reserve(roots.size());
    for (auto& root : roots) {
        outputs.push_back(std::unique_ptr<Operator>(new OutputOperator(std::move(root))));
    }
    return outputs;
}

struct ProducedPipeline {
    std::shared_ptr<ExchangeBuffer> buffer;
    std::string pipeline_id;
};

std::string ChildPipelineId(const std::string& parent, std::string suffix) {
    return parent.empty() ? std::move(suffix) : absl::StrCat(parent, "-", suffix);
}

absl::StatusOr<ProducedPipeline> AddProducerPipelineForPlan(
    const std::shared_ptr<plan::PlanNode>& plan_node,
    std::string id,
    std::string role,
    ExecutionTask* task);

absl::StatusOr<ProducedPipeline> AddJoinProducerPipeline(
    const std::shared_ptr<plan::PlanNode>& join_plan,
    std::string id,
    std::string role,
    ExecutionTask* task) {
    if (join_plan == nullptr || join_plan->kind != plan::PlanNodeKind::Join ||
        join_plan->inputs.size() != 2 || join_plan->inputs[0] == nullptr ||
        join_plan->inputs[1] == nullptr) {
        return absl::InvalidArgumentError("join execution task requires two inputs");
    }

    const bool build_left = join_plan->join().build_side == plan::JoinBuildSide::Left;
    auto left_or = AddProducerPipelineForPlan(join_plan->inputs[0], ChildPipelineId(id, "left"),
                                              build_left ? "build" : "probe", task);
    if (!left_or.ok()) {
        return left_or.status();
    }
    auto right_or = AddProducerPipelineForPlan(join_plan->inputs[1], ChildPipelineId(id, "right"),
                                               build_left ? "probe" : "build", task);
    if (!right_or.ok()) {
        return right_or.status();
    }

    auto output_buffer = std::make_shared<ExchangeBuffer>();
    std::unique_ptr<Operator> left_source(new ExchangeSourceOperator(left_or->buffer));
    std::unique_ptr<Operator> right_source(new ExchangeSourceOperator(right_or->buffer));
    std::vector<std::string> root_operators = {left_source->name(), right_source->name()};
    std::unique_ptr<Operator> join(
        new LocalHashJoinOperator(join_plan, std::move(left_source), std::move(right_source)));
    root_operators.push_back(join->name());
    std::unique_ptr<Operator> sink(new ExchangeSinkOperator(std::move(join), output_buffer));
    root_operators.push_back(sink->name());

    task->pipelines.push_back(MakePipeline(std::move(id), "join producer", std::move(role),
                                           {left_or->pipeline_id, right_or->pipeline_id},
                                           std::move(root_operators), std::move(sink)));
    return ProducedPipeline{.buffer = std::move(output_buffer),
                            .pipeline_id = task->pipelines.back().id};
}

absl::StatusOr<ProducedPipeline> AddProducerPipelineForPlan(
    const std::shared_ptr<plan::PlanNode>& plan_node,
    std::string id,
    std::string role,
    ExecutionTask* task) {
    if (task == nullptr) {
        return absl::InvalidArgumentError("missing execution task");
    }
    if (plan_node != nullptr && plan_node->kind == plan::PlanNodeKind::Join) {
        return AddJoinProducerPipeline(plan_node, std::move(id), std::move(role), task);
    }

    auto planned_or = BuildOperator(plan_node);
    if (!planned_or.ok()) {
        return planned_or.status();
    }
    auto output_buffer = std::make_shared<ExchangeBuffer>();
    std::vector<std::string> operators = std::move(planned_or->operators);
    if (!planned_or->driver_roots.empty()) {
        output_buffer->SetProducerCount(planned_or->driver_roots.size());
        auto sinks =
            WrapDriverRootsWithExchangeSinks(std::move(planned_or->driver_roots), output_buffer);
        operators.push_back("ExchangeSinkOperator");
        task->pipelines.push_back(MakePipeline(std::move(id), "producer", std::move(role), {},
                                               std::move(operators), nullptr, std::move(sinks)));
        return ProducedPipeline{.buffer = std::move(output_buffer),
                                .pipeline_id = task->pipelines.back().id};
    }
    std::unique_ptr<Operator> sink(
        new ExchangeSinkOperator(std::move(planned_or->root), output_buffer));
    operators.push_back(sink->name());
    task->pipelines.push_back(MakePipeline(std::move(id), "producer", std::move(role), {},
                                           std::move(operators), std::move(sink)));
    return ProducedPipeline{.buffer = std::move(output_buffer),
                            .pipeline_id = task->pipelines.back().id};
}

absl::StatusOr<ExecutionTask> BuildJoinExecutionTask(
    const std::shared_ptr<plan::PlanNode>& join_plan) {
    if (join_plan == nullptr || join_plan->kind != plan::PlanNodeKind::Join ||
        join_plan->inputs.size() != 2 || join_plan->inputs[0] == nullptr ||
        join_plan->inputs[1] == nullptr) {
        return absl::InvalidArgumentError("join execution task requires two inputs");
    }

    ExecutionTask task;
    const bool build_left = join_plan->join().build_side == plan::JoinBuildSide::Left;
    auto left_or = AddProducerPipelineForPlan(join_plan->inputs[0], "join-left",
                                              build_left ? "build" : "probe", &task);
    if (!left_or.ok()) {
        return left_or.status();
    }
    auto right_or = AddProducerPipelineForPlan(join_plan->inputs[1], "join-right",
                                               build_left ? "probe" : "build", &task);
    if (!right_or.ok()) {
        return right_or.status();
    }

    std::unique_ptr<Operator> left_source(new ExchangeSourceOperator(left_or->buffer));
    std::unique_ptr<Operator> right_source(new ExchangeSourceOperator(right_or->buffer));
    std::vector<std::string> root_operators = {left_source->name(), right_source->name()};
    std::unique_ptr<Operator> join(
        new LocalHashJoinOperator(join_plan, std::move(left_source), std::move(right_source)));
    root_operators.push_back(join->name());
    std::unique_ptr<Operator> output(new OutputOperator(std::move(join)));
    root_operators.push_back(output->name());

    task.pipelines.push_back(MakePipeline("main", "main", "root",
                                          {left_or->pipeline_id, right_or->pipeline_id},
                                          std::move(root_operators), std::move(output)));
    return task;
}

absl::StatusOr<ExecutionTask> BuildExchangeExecutionTask(
    const std::shared_ptr<plan::PlanNode>& exchange_plan) {
    if (exchange_plan == nullptr || exchange_plan->kind != plan::PlanNodeKind::Exchange ||
        exchange_plan->inputs.size() != 1 || exchange_plan->inputs[0] == nullptr) {
        return absl::InvalidArgumentError("exchange execution task requires one input");
    }

    ExecutionTask task;
    auto input_or =
        AddProducerPipelineForPlan(exchange_plan->inputs[0], "exchange-input", "source", &task);
    if (!input_or.ok()) {
        return input_or.status();
    }
    std::unique_ptr<Operator> source(new ExchangeSourceOperator(input_or->buffer));
    std::vector<std::string> operators = {source->name()};
    std::unique_ptr<Operator> exchange(new ExchangeOperator(exchange_plan, std::move(source)));
    operators.push_back(exchange->name());
    std::unique_ptr<Operator> output(new OutputOperator(std::move(exchange)));
    operators.push_back(output->name());
    task.pipelines.push_back(MakePipeline("main", "main", "root", {input_or->pipeline_id},
                                          std::move(operators), std::move(output)));
    return task;
}

} // namespace

absl::StatusOr<ExecutionTask> PhysicalPlanner::Plan(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto fast_or = optimizer::FastCostBasedOptimizer().OptimizeWithTrace(logical_plan);
    if (!fast_or.ok()) {
        return fast_or.status();
    }
    if (fast_or->rbo_result.plan != nullptr &&
        fast_or->rbo_result.plan->kind == plan::PlanNodeKind::Join) {
        auto cbo_or = optimizer::DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);
        if (!cbo_or.ok()) {
            return cbo_or.status();
        }
        return BuildJoinExecutionTask(cbo_or->rbo_result.plan);
    }
    if (fast_or->rbo_result.plan != nullptr &&
        fast_or->rbo_result.plan->kind == plan::PlanNodeKind::Exchange) {
        return BuildExchangeExecutionTask(fast_or->rbo_result.plan);
    }

    auto planned_or = BuildOperator(logical_plan);
    if (!planned_or.ok()) {
        return planned_or.status();
    }
    if (!planned_or->driver_roots.empty()) {
        std::vector<std::string> operators = std::move(planned_or->operators);
        auto outputs = WrapDriverRootsWithOutput(std::move(planned_or->driver_roots));
        operators.push_back("OutputOperator");
        ExecutionTask task;
        task.pipelines.push_back(MakePipeline("main", "main", "root", {}, std::move(operators),
                                              nullptr, std::move(outputs)));
        return task;
    }
    std::unique_ptr<Operator> output(new OutputOperator(std::move(planned_or->root)));
    planned_or->operators.push_back(output->name());

    ExecutionTask task;
    Pipeline pipeline;
    pipeline.id = "main";
    pipeline.name = "main";
    pipeline.role = "root";
    pipeline.operators = std::move(planned_or->operators);
    pipeline.root = std::move(output);
    task.pipelines.push_back(std::move(pipeline));
    return task;
}

void ResetPipelineStats(const std::shared_ptr<Pipeline::Stats>& stats) {
    if (stats == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(stats->mu);
    stats->pages = 0;
    stats->rows = 0;
    stats->blocked = false;
    stats->finished = false;
    stats->error.clear();
}

void AddPipelineStatsPage(const std::shared_ptr<Pipeline::Stats>& stats, const Page& page) {
    if (stats == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(stats->mu);
    ++stats->pages;
    stats->rows += page.row_count();
}

void FinishPipelineStats(const std::shared_ptr<Pipeline::Stats>& stats) {
    if (stats == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(stats->mu);
    stats->finished = true;
}

void FailPipelineStats(const std::shared_ptr<Pipeline::Stats>& stats, const absl::Status& status) {
    if (stats == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(stats->mu);
    stats->blocked = status.code() == absl::StatusCode::kUnavailable;
    stats->error = status.ToString();
    stats->finished = true;
}

Driver::Driver(Pipeline pipeline) : pipeline_(std::move(pipeline)) {}

absl::StatusOr<Value> Driver::Run() const {
    if (pipeline_.root == nullptr) {
        return absl::InvalidArgumentError("pipeline has no root operator");
    }
    std::optional<Page> output;
    while (true) {
        auto page_or = pipeline_.root->NextPage();
        if (!page_or.ok()) {
            FailPipelineStats(pipeline_.stats, page_or.status());
            return page_or.status();
        }
        if (!page_or->has_value()) {
            break;
        }
        AddPipelineStatsPage(pipeline_.stats, page_or->value());
        if (!output.has_value()) {
            output = std::move(page_or->value());
            continue;
        }
        AppendPage(&*output, std::move(page_or->value()));
    }
    FinishPipelineStats(pipeline_.stats);
    if (!output.has_value()) {
        return Value::table_stream("", {});
    }
    Value value = value_from_page(*output);
    value.as_table_mut().materialized = true;
    return value;
}

std::vector<DriverTask> DriverFactory::CreateTasks(size_t pipeline_index, Pipeline pipeline) {
    std::vector<DriverTask> tasks;
    auto make_task = [&](size_t driver_id, std::unique_ptr<Operator> root) {
        Pipeline driver_pipeline;
        driver_pipeline.id = pipeline.id;
        driver_pipeline.name = pipeline.name;
        driver_pipeline.role = pipeline.role;
        driver_pipeline.dependencies = pipeline.dependencies;
        driver_pipeline.operators = pipeline.operators;
        driver_pipeline.root = std::move(root);
        driver_pipeline.stats = pipeline.stats;

        DriverTask task;
        task.pipeline_index = pipeline_index;
        task.driver_id = driver_id;
        task.pipeline_id = driver_pipeline.id;
        task.role = driver_pipeline.role;
        task.pipeline = std::move(driver_pipeline);
        tasks.push_back(std::move(task));
    };

    if (!pipeline.driver_roots.empty()) {
        tasks.reserve(pipeline.driver_roots.size());
        for (size_t index = 0; index < pipeline.driver_roots.size(); ++index) {
            make_task(index, std::move(pipeline.driver_roots[index]));
        }
        return tasks;
    }

    make_task(0, std::move(pipeline.root));
    return tasks;
}

ExecutionProfile BuildExecutionProfile(const std::vector<PipelineProfile>& pipeline_templates,
                                       const std::vector<std::shared_ptr<Pipeline::Stats>>& stats) {
    ExecutionProfile profile;
    profile.pipelines = pipeline_templates;
    for (size_t index = 0; index < profile.pipelines.size() && index < stats.size(); ++index) {
        if (stats[index] == nullptr) {
            continue;
        }
        std::lock_guard<std::mutex> lock(stats[index]->mu);
        profile.pipelines[index].pages = stats[index]->pages;
        profile.pipelines[index].rows = stats[index]->rows;
        profile.pipelines[index].blocked = stats[index]->blocked;
        profile.pipelines[index].finished = stats[index]->finished;
        profile.pipelines[index].error = stats[index]->error;
    }
    return profile;
}

bool IsBlockingOperatorName(const std::string& name) {
    return name == "SortOperator" || name == "GroupOperator" || name == "AggregateOperator" ||
           name == "DistinctOperator" || name == "LocalHashJoinOperator" ||
           name == "ExchangeOperator" || name == "MaterializeOperator";
}

bool HasBlockingOperator(const std::vector<std::string>& operators) {
    for (const auto& name : operators) {
        if (IsBlockingOperatorName(name)) {
            return true;
        }
    }
    return false;
}

absl::StatusOr<SchedulerResult> Scheduler::RunWithProfile(ExecutionTask task) const {
    if (task.pipelines.empty()) {
        return absl::InvalidArgumentError("execution task has no pipelines");
    }
    std::vector<PipelineProfile> pipeline_profiles;
    std::vector<std::shared_ptr<Pipeline::Stats>> pipeline_stats;
    pipeline_profiles.reserve(task.pipelines.size());
    pipeline_stats.reserve(task.pipelines.size());
    for (const auto& pipeline : task.pipelines) {
        pipeline_profiles.push_back(PipelineProfile{
            .id = pipeline.id,
            .name = pipeline.name,
            .role = pipeline.role,
            .dependencies = pipeline.dependencies,
            .operators = pipeline.operators,
            .blocking = HasBlockingOperator(pipeline.operators),
        });
        pipeline_stats.push_back(pipeline.stats);
    }

    std::unordered_map<std::string, size_t> pipeline_indexes;
    for (size_t index = 0; index < task.pipelines.size(); ++index) {
        if (!task.pipelines[index].id.empty()) {
            pipeline_indexes.emplace(task.pipelines[index].id, index);
        }
    }

    std::vector<size_t> indegree(task.pipelines.size(), 0);
    std::vector<std::vector<size_t>> dependents(task.pipelines.size());
    for (size_t index = 0; index < task.pipelines.size(); ++index) {
        for (const auto& dependency : task.pipelines[index].dependencies) {
            auto it = pipeline_indexes.find(dependency);
            if (it == pipeline_indexes.end()) {
                return absl::InvalidArgumentError(
                    absl::StrCat("pipeline depends on missing pipeline: ", dependency));
            }
            ++indegree[index];
            dependents[it->second].push_back(index);
        }
    }
    std::queue<size_t> ready;
    for (size_t index = 0; index < indegree.size(); ++index) {
        if (indegree[index] == 0) {
            ready.push(index);
        }
    }
    size_t visited = 0;
    while (!ready.empty()) {
        const size_t index = ready.front();
        ready.pop();
        ++visited;
        for (size_t dependent : dependents[index]) {
            --indegree[dependent];
            if (indegree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }
    if (visited != task.pipelines.size()) {
        return absl::FailedPreconditionError("execution task pipeline dependency cycle");
    }

    std::optional<TableValue> output;
    bool ran_pipeline = false;

    struct RunningPipeline {
        size_t index = 0;
        std::string id;
        std::string role;
        size_t driver_id = 0;
        std::future<absl::StatusOr<Value>> value;
    };

    std::vector<DriverTask> driver_tasks;
    for (size_t index = 0; index < task.pipelines.size(); ++index) {
        ResetPipelineStats(task.pipelines[index].stats);
        if (task.pipelines[index].root == nullptr && task.pipelines[index].driver_roots.empty()) {
            FinishPipelineStats(task.pipelines[index].stats);
            continue;
        }
        auto tasks = DriverFactory::CreateTasks(index, std::move(task.pipelines[index]));
        driver_tasks.insert(driver_tasks.end(), std::make_move_iterator(tasks.begin()),
                            std::make_move_iterator(tasks.end()));
    }

    std::vector<RunningPipeline> running;
    running.reserve(driver_tasks.size());
    TaskExecutor executor(std::max<size_t>(1, driver_tasks.size()));
    for (auto& item : driver_tasks) {
        ran_pipeline = true;
        RunningPipeline running_pipeline;
        running_pipeline.index = item.pipeline_index;
        running_pipeline.id = item.pipeline_id;
        running_pipeline.role = item.role;
        running_pipeline.driver_id = item.driver_id;
        running_pipeline.value = executor.Submit([driver_task = std::move(item)]() mutable {
            return Driver(std::move(driver_task.pipeline)).Run();
        });
        running.push_back(std::move(running_pipeline));
    }

    for (auto& running_pipeline : running) {
        auto value_or = running_pipeline.value.get();
        if (!value_or.ok()) {
            return value_or.status();
        }
        if (running_pipeline.role != "root" && running_pipeline.id != "main" &&
            task.pipelines.size() != 1) {
            continue;
        }
        const auto& table = value_or->as_table();
        if (!output.has_value()) {
            output = table;
            continue;
        }
        output->tables.insert(output->tables.end(), table.tables.begin(), table.tables.end());
        output->rows.insert(output->rows.end(), table.rows.begin(), table.rows.end());
    }
    if (!ran_pipeline) {
        return absl::InvalidArgumentError("execution task has no runnable pipelines");
    }
    if (!output.has_value()) {
        SchedulerResult result;
        result.value = Value::table_stream("", {});
        result.profile = BuildExecutionProfile(pipeline_profiles, pipeline_stats);
        return result;
    }
    Value value = Value::table_stream(output->bucket, output->tables, output->range_start,
                                      output->range_stop, output->result_name);
    value.as_table_mut().plan = output->plan;
    value.as_table_mut().materialized = true;
    SchedulerResult result;
    result.value = std::move(value);
    result.profile = BuildExecutionProfile(pipeline_profiles, pipeline_stats);
    return result;
}

absl::StatusOr<Value> Scheduler::Run(ExecutionTask task) const {
    auto result_or = RunWithProfile(std::move(task));
    if (!result_or.ok()) {
        return result_or.status();
    }
    return std::move(result_or->value);
}

absl::StatusOr<SchedulerResult> PhysicalExecutor::ExecuteWithProfile(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto operator_or = PhysicalPlanner().Plan(logical_plan);
    if (!operator_or.ok()) {
        return operator_or.status();
    }
    return Scheduler().RunWithProfile(std::move(*operator_or));
}

absl::StatusOr<Value> PhysicalExecutor::Execute(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto result_or = ExecuteWithProfile(logical_plan);
    if (!result_or.ok()) {
        return result_or.status();
    }
    return std::move(result_or->value);
}

std::string FormatPipelinePlan(const std::shared_ptr<plan::PlanNode>& logical_plan) {
    auto task_or = PhysicalPlanner().Plan(logical_plan);
    if (!task_or.ok()) {
        return task_or.status().ToString() + "\n";
    }
    std::ostringstream out;
    out << "PipelinePlan\n";
    for (const auto& pipeline : task_or->pipelines) {
        out << "- Pipeline(id=\"" << pipeline.id << "\", role=\"" << pipeline.role << "\"";
        if (!pipeline.dependencies.empty()) {
            out << ", depends_on=" << plan::StringList(pipeline.dependencies);
        }
        out << ", blocking=" << (HasBlockingOperator(pipeline.operators) ? "true" : "false")
            << ", operators=" << plan::StringList(pipeline.operators) << ")\n";
    }
    return out.str();
}

std::string FormatExecutionProfile(const ExecutionProfile& profile) {
    std::ostringstream out;
    out << "ExecutionProfile\n";
    for (const auto& pipeline : profile.pipelines) {
        out << "- Pipeline(id=\"" << pipeline.id << "\", role=\"" << pipeline.role << "\"";
        if (!pipeline.dependencies.empty()) {
            out << ", depends_on=" << plan::StringList(pipeline.dependencies);
        }
        out << ", blocking=" << (pipeline.blocking ? "true" : "false")
            << ", pages=" << pipeline.pages << ", rows=" << pipeline.rows
            << ", blocked=" << (pipeline.blocked ? "true" : "false")
            << ", finished=" << (pipeline.finished ? "true" : "false");
        if (!pipeline.error.empty()) {
            out << ", error=\"" << pipeline.error << "\"";
        }
        out << ")\n";
    }
    return out.str();
}

} // namespace pl::flux::execution
