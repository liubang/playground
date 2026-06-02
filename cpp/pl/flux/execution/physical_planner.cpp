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
// Created: 2026/06/02 22:23

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/connector_registry.h"
#include "cpp/pl/flux/connector/connector_runtime.h"
#include "cpp/pl/flux/execution/accumulator.h"
#include "cpp/pl/flux/execution/page_budget.h"
#include "cpp/pl/flux/execution/physical_executor.h"
#include "cpp/pl/flux/execution/physical_executor_internal.h"
#include "cpp/pl/flux/execution/task_executor.h"
#include "cpp/pl/flux/optimizer/cbo.h"
#include "cpp/pl/flux/runtime/runtime_builtin_aggregate_helpers.h"
#include "cpp/pl/flux/runtime/runtime_builtin_table_helpers.h"

namespace pl::flux::execution {
namespace {
using namespace detail;
using Clock = std::chrono::steady_clock;

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

bool page_row_time_in_range(const PageChunk& chunk, size_t row_index, const plan::RangeSpec& spec) {
    const Value* value = PageChunkValueAt(chunk, row_index, "_time");
    if (value == nullptr) {
        return false;
    }
    const std::string literal =
        value->type() == Value::Type::Time ? value->as_time().literal : value->string();
    if (!spec.start.empty() && literal < spec.start) {
        return false;
    }
    return !spec.stop.has_value() || literal < *spec.stop;
}

Page page_with_plan(Page page, const std::shared_ptr<plan::PlanNode>& plan) {
    page.plan = plan;
    page.materialized = true;
    return page;
}

connector::SourceSpec source_spec_from_plan(const optimizer::PushdownPlan& plan) {
    return connector::SourceSpec{.source = plan.source.source,
                                 .driver = plan.source.driver,
                                 .dsn = plan.source.dsn,
                                 .table = plan.source.table};
}

connector::SourceSpec source_spec_from_split(const connector::ConnectorSplit& split) {
    return connector::SourceSpec{.source = split.table.source,
                                 .driver = split.table.driver,
                                 .dsn = split.table.dsn,
                                 .table = split.table.table};
}

absl::Status validate_executable_pushdown_plan(const optimizer::PushdownPlan& plan) {
    if (plan.source.source.empty() || plan.source.driver.empty()) {
        return absl::InvalidArgumentError("pushdown plan has no source");
    }
    if (!optimizer::CanExecutePushdownPlan(plan)) {
        return absl::InvalidArgumentError("pushdown plan is not executable");
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

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void add_page_to_split_stats(connector::ConnectorSplitStats* stats, const Page& page) {
    if (stats == nullptr) {
        return;
    }
    ++stats->pages_produced;
    stats->rows_produced += page.row_count();
    stats->bytes_produced += EstimatePageBytes(page);
}

void copy_source_profile(connector::ConnectorSplitStats* stats,
                         const connector::ConnectorSplitStats& source) {
    if (stats == nullptr) {
        return;
    }
    stats->connect_time_ms = source.connect_time_ms;
    stats->schema_time_ms = source.schema_time_ms;
    stats->sql_build_time_ms = source.sql_build_time_ms;
    stats->execute_time_ms = source.execute_time_ms;
    stats->read_time_ms = source.read_time_ms;
    stats->decode_time_ms = source.decode_time_ms;
    stats->page_build_time_ms = source.page_build_time_ms;
}

bool page_row_less(const std::shared_ptr<ObjectValue>& lhs,
                   const std::shared_ptr<ObjectValue>& rhs,
                   const std::vector<plan::SortKey>& keys) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs != nullptr;
    }
    for (const auto& key : keys) {
        const int cmp = compare_values(lhs->lookup(key.column), rhs->lookup(key.column));
        if (cmp != 0) {
            return key.desc ? cmp > 0 : cmp < 0;
        }
    }
    return false;
}

std::vector<std::shared_ptr<ObjectValue>> rows_from_page(const Page& page) {
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve(page.row_count());
    for (const auto& chunk : page.chunks) {
        for (size_t row_index = 0; row_index < chunk.row_count; ++row_index) {
            rows.push_back(RowFromPageChunk(chunk, row_index));
        }
    }
    return rows;
}

absl::StatusOr<Page> apply_sort_page(const Page& input,
                                     const std::shared_ptr<plan::PlanNode>& plan) {
    auto rows = rows_from_page(input);
    std::ranges::stable_sort(rows, [&](const auto& lhs, const auto& rhs) {
        return page_row_less(lhs, rhs, plan->sort().keys);
    });
    Page output = PageFromRows(input.bucket, std::move(rows));
    output.range_start = input.range_start;
    output.range_stop = input.range_stop;
    output.result_name = input.result_name;
    return page_with_plan(std::move(output), plan);
}

absl::StatusOr<Page> apply_topn_page(const Page& input,
                                     const std::shared_ptr<plan::PlanNode>& sort_plan,
                                     size_t limit) {
    if (limit == 0) {
        return page_with_plan(PageFromRows(input.bucket, {}), sort_plan);
    }
    const auto& keys = sort_plan->sort().keys;
    auto worse_first = [&](const auto& lhs, const auto& rhs) {
        return page_row_less(lhs, rhs, keys);
    };
    std::priority_queue<std::shared_ptr<ObjectValue>,
                        std::vector<std::shared_ptr<ObjectValue>>,
                        decltype(worse_first)>
        heap(worse_first);
    for (const auto& row : rows_from_page(input)) {
        if (row == nullptr) {
            continue;
        }
        if (heap.size() < limit) {
            heap.push(row);
            continue;
        }
        if (page_row_less(row, heap.top(), keys)) {
            heap.pop();
            heap.push(row);
        }
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve(heap.size());
    while (!heap.empty()) {
        rows.push_back(heap.top());
        heap.pop();
    }
    std::ranges::stable_sort(
        rows, [&](const auto& lhs, const auto& rhs) { return page_row_less(lhs, rhs, keys); });
    Page output = PageFromRows(input.bucket, std::move(rows));
    output.range_start = input.range_start;
    output.range_stop = input.range_stop;
    output.result_name = input.result_name;
    return page_with_plan(std::move(output), sort_plan);
}

Value value_from_page(const Page& page) {
    TableValue table = TableValueFromPage(page);
    Value value = Value::table_stream(table.bucket,
                                      std::move(table.tables),
                                      table.range_start,
                                      table.range_stop,
                                      table.result_name);
    value.as_table_mut().plan = table.plan;
    value.as_table_mut().materialized = table.materialized;
    return value;
}

Page page_from_value(const Value& value) {
    return PageFromTableValue(value.as_table());
}

absl::StatusOr<Page> apply_range_page(const Page& input,
                                      const std::shared_ptr<plan::PlanNode>& plan) {
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
                ColumnVector{.name = source_column.name, .type = source_column.type, .values = {}});
        }
        for (size_t row_index = 0; row_index < source.row_count; ++row_index) {
            if (!page_row_time_in_range(source, row_index, plan->range())) {
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

absl::StatusOr<Page> apply_filter_page(const Page& input,
                                       const std::shared_ptr<plan::PlanNode>& plan) {
    auto status = ValidatePage(input);
    if (!status.ok()) {
        return status;
    }
    std::vector<PageChunk> chunks;
    chunks.reserve(input.chunks.size());
    for (const auto& source : input.chunks) {
        PageSchema schema = SchemaFromPageChunk(source);
        struct IndexedPredicate {
            const plan::PredicateSpec* predicate = nullptr;
            size_t column_index = 0;
            Value literal;
        };
        std::vector<IndexedPredicate> predicates;
        predicates.reserve(plan->filter().predicates.size());
        for (const auto& predicate : plan->filter().predicates) {
            auto source_index = schema.FindColumn(predicate.column);
            if (!source_index.has_value()) {
                return absl::InvalidArgumentError(absl::StrCat(
                    "memory filter references unavailable column: ", predicate.column));
            }
            predicates.push_back(IndexedPredicate{
                .predicate = &predicate,
                .column_index = *source_index,
                .literal = literal_value(predicate.literal),
            });
        }

        PageChunk chunk;
        chunk.group_key = source.group_key;
        chunk.row_count = 0;
        chunk.columns.reserve(source.columns.size());
        for (const auto& source_column : source.columns) {
            chunk.columns.push_back(
                ColumnVector{.name = source_column.name, .type = source_column.type, .values = {}});
            chunk.columns.back().values.reserve(source_column.values.size());
        }
        for (size_t row_index = 0; row_index < source.row_count; ++row_index) {
            bool keep = true;
            for (const auto& item : predicates) {
                const auto& column = source.columns[item.column_index];
                if (row_index >= column.values.size()) {
                    keep = false;
                    break;
                }
                const int cmp = compare_values(&column.values[row_index], &item.literal);
                bool matches = false;
                switch (item.predicate->op) {
                    case plan::PredicateOp::Eq:
                        matches = cmp == 0;
                        break;
                    case plan::PredicateOp::NotEq:
                        matches = cmp != 0;
                        break;
                    case plan::PredicateOp::Lt:
                        matches = cmp < 0;
                        break;
                    case plan::PredicateOp::Lte:
                        matches = cmp <= 0;
                        break;
                    case plan::PredicateOp::Gt:
                        matches = cmp > 0;
                        break;
                    case plan::PredicateOp::Gte:
                        matches = cmp >= 0;
                        break;
                }
                if (!matches) {
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

absl::StatusOr<Page> apply_project_page(const Page& input,
                                        const std::shared_ptr<plan::PlanNode>& plan) {
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

absl::StatusOr<Page> apply_exchange_page(const Page& input,
                                         const std::shared_ptr<plan::PlanNode>& plan) {
    auto status = ValidatePage(input);
    if (!status.ok()) {
        return status;
    }
    if (plan->exchange().kind == plan::ExchangeKind::Gather ||
        plan->exchange().partition_keys.empty()) {
        return page_with_plan(input, plan);
    }

    std::vector<TableChunk> chunks;
    std::unordered_map<std::string, size_t> partition_index;
    std::vector<std::string> partition_order;
    for (const auto& page_chunk : input.chunks) {
        std::vector<std::string> columns;
        columns.reserve(page_chunk.columns.size());
        for (const auto& column : page_chunk.columns) {
            columns.push_back(column.name);
        }
        for (size_t row_index = 0; row_index < page_chunk.row_count; ++row_index) {
            auto row = RowFromPageChunk(page_chunk, row_index);
            auto key = exchange_partition_key(*row, plan->exchange().partition_keys);
            if (!key.has_value()) {
                continue;
            }
            auto it = partition_index.find(*key);
            if (it == partition_index.end()) {
                const size_t index = chunks.size();
                partition_index.emplace(*key, index);
                partition_order.push_back(*key);
                TableChunk chunk;
                chunk.columns = columns;
                chunks.push_back(std::move(chunk));
                it = partition_index.find(partition_order.back());
            }
            chunks[it->second].rows.push_back(std::move(row));
        }
    }
    Page output = PageFromTableChunks(
        input.bucket, chunks, input.range_start, input.range_stop, input.result_name);
    return page_with_plan(std::move(output), plan);
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

std::string local_joined_property_name(const std::string& table_name, const std::string& column) {
    return absl::StrCat(column, "_", table_name);
}

std::vector<std::pair<std::string, Value>> local_join_group_properties(
    const ObjectValue* left,
    const ObjectValue* right,
    const plan::JoinSpec& spec,
    const std::unordered_set<std::string>& overlapping_columns) {
    const std::unordered_set<std::string> on_columns(spec.on.begin(), spec.on.end());
    std::vector<std::pair<std::string, Value>> props;
    std::unordered_set<std::string> inserted;
    auto append = [&](const ObjectValue* row, const std::string& table_name) {
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
                output_key = local_joined_property_name(table_name, key);
            }
            if (inserted.insert(output_key).second) {
                props.emplace_back(output_key, value);
            }
        }
    };
    append(left, spec.left_name);
    append(right, spec.right_name);
    return props;
}

std::shared_ptr<ObjectValue> local_join_row(const ObjectValue* left,
                                            const std::vector<std::string>& left_columns,
                                            const ObjectValue* right,
                                            const std::vector<std::string>& right_columns,
                                            const plan::JoinSpec& spec) {
    const std::unordered_set<std::string> on_columns(spec.on.begin(), spec.on.end());
    const std::unordered_set<std::string> right_column_set(right_columns.begin(),
                                                           right_columns.end());
    std::unordered_set<std::string> overlapping_columns;
    for (const auto& column : left_columns) {
        if (on_columns.count(column) == 0 && right_column_set.count(column) != 0) {
            overlapping_columns.insert(column);
        }
    }
    auto group_props = local_join_group_properties(left, right, spec, overlapping_columns);
    std::vector<std::pair<std::string, Value>> props;
    props.reserve(left_columns.size() + right_columns.size() + group_props.size() + 1);
    for (const auto& [key, value] : group_props) {
        props.emplace_back(key, value);
    }

    auto append = [&](const std::string& column, const Value* value) {
        const bool already_present = std::ranges::any_of(
            props, [&](const auto& property) { return property.first == column; });
        if (!already_present) {
            props.emplace_back(column, value == nullptr ? Value::null() : *value);
        }
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
        const std::string output_name = overlapping_columns.count(column) == 0
                                            ? column
                                            : local_joined_property_name(spec.left_name, column);
        append(output_name, left == nullptr ? nullptr : left->lookup(column));
    }
    for (const auto& column : right_columns) {
        if (column == "_group" || on_columns.count(column) != 0) {
            continue;
        }
        const std::string output_name = overlapping_columns.count(column) == 0
                                            ? column
                                            : local_joined_property_name(spec.right_name, column);
        append(output_name, right == nullptr ? nullptr : right->lookup(column));
    }
    if (!group_props.empty()) {
        props.emplace_back("_group", Value::object(std::move(group_props)));
    }
    return std::make_shared<ObjectValue>(std::move(props));
}

std::string local_chunk_group_key(const TableChunk& chunk) {
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

std::vector<const ObjectValue*> local_rows(const std::vector<const TableChunk*>& chunks) {
    std::vector<const ObjectValue*> rows;
    for (const auto* chunk : chunks) {
        for (const auto& row : chunk->rows) {
            if (row != nullptr) {
                rows.push_back(row.get());
            }
        }
    }
    return rows;
}

TableChunk local_hash_join_group(const std::vector<const ObjectValue*>& left_rows,
                                 const std::vector<const ObjectValue*>& right_rows,
                                 const std::vector<std::string>& left_columns,
                                 const std::vector<std::string>& right_columns,
                                 const plan::JoinSpec& spec) {
    auto left_null = null_row_for_columns(left_columns);
    auto right_null = null_row_for_columns(right_columns);
    TableChunk chunk;
    if (spec.build_side == plan::JoinBuildSide::Left) {
        std::unordered_map<std::string, std::vector<size_t>> left_rows_by_key;
        left_rows_by_key.reserve(left_rows.size());
        for (size_t row_index = 0; row_index < left_rows.size(); ++row_index) {
            auto key = local_join_key(*left_rows[row_index], spec.on);
            if (key.has_value()) {
                left_rows_by_key[*key].push_back(row_index);
            }
        }

        std::vector<bool> matched_left(left_rows.size(), false);
        for (const auto* right_row : right_rows) {
            bool matched = false;
            auto key = local_join_key(*right_row, spec.on);
            if (key.has_value()) {
                auto it = left_rows_by_key.find(*key);
                if (it != left_rows_by_key.end()) {
                    matched = true;
                    for (size_t left_index : it->second) {
                        matched_left[left_index] = true;
                        chunk.rows.push_back(local_join_row(
                            left_rows[left_index], left_columns, right_row, right_columns, spec));
                    }
                }
            }
            if (!matched &&
                (spec.method == plan::JoinMethod::Right || spec.method == plan::JoinMethod::Full)) {
                chunk.rows.push_back(
                    local_join_row(left_null.get(), left_columns, right_row, right_columns, spec));
            }
        }
        if (spec.method == plan::JoinMethod::Left || spec.method == plan::JoinMethod::Full) {
            for (size_t left_index = 0; left_index < left_rows.size(); ++left_index) {
                if (matched_left[left_index]) {
                    continue;
                }
                chunk.rows.push_back(local_join_row(
                    left_rows[left_index], left_columns, right_null.get(), right_columns, spec));
            }
        }
    } else {
        std::unordered_map<std::string, std::vector<size_t>> right_rows_by_key;
        right_rows_by_key.reserve(right_rows.size());
        for (size_t row_index = 0; row_index < right_rows.size(); ++row_index) {
            auto key = local_join_key(*right_rows[row_index], spec.on);
            if (key.has_value()) {
                right_rows_by_key[*key].push_back(row_index);
            }
        }

        std::vector<bool> matched_right(right_rows.size(), false);
        for (const auto* left_row : left_rows) {
            bool matched = false;
            auto key = local_join_key(*left_row, spec.on);
            if (key.has_value()) {
                auto it = right_rows_by_key.find(*key);
                if (it != right_rows_by_key.end()) {
                    matched = true;
                    for (size_t right_index : it->second) {
                        matched_right[right_index] = true;
                        chunk.rows.push_back(local_join_row(
                            left_row, left_columns, right_rows[right_index], right_columns, spec));
                    }
                }
            }
            if (!matched &&
                (spec.method == plan::JoinMethod::Left || spec.method == plan::JoinMethod::Full)) {
                chunk.rows.push_back(
                    local_join_row(left_row, left_columns, right_null.get(), right_columns, spec));
            }
        }
        if (spec.method == plan::JoinMethod::Right || spec.method == plan::JoinMethod::Full) {
            for (size_t right_index = 0; right_index < right_rows.size(); ++right_index) {
                if (matched_right[right_index]) {
                    continue;
                }
                chunk.rows.push_back(local_join_row(
                    left_null.get(), left_columns, right_rows[right_index], right_columns, spec));
            }
        }
    }
    return chunk;
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

    std::unordered_map<std::string, std::vector<const TableChunk*>> right_chunks_by_group;
    right_chunks_by_group.reserve(right_table.table_count());
    for (const auto& chunk : right_table.tables) {
        right_chunks_by_group[local_chunk_group_key(chunk)].push_back(&chunk);
    }

    std::unordered_map<std::string, std::vector<const TableChunk*>> left_chunks_by_group;
    left_chunks_by_group.reserve(left_table.table_count());
    for (const auto& chunk : left_table.tables) {
        left_chunks_by_group[local_chunk_group_key(chunk)].push_back(&chunk);
    }

    std::vector<TableChunk> chunks;
    std::unordered_set<std::string> processed_groups;
    for (const auto& [group_key, left_chunks] : left_chunks_by_group) {
        processed_groups.insert(group_key);
        const auto right_it = right_chunks_by_group.find(group_key);
        if (right_it == right_chunks_by_group.end() && spec.method != plan::JoinMethod::Left &&
            spec.method != plan::JoinMethod::Full) {
            continue;
        }
        auto chunk = local_hash_join_group(local_rows(left_chunks),
                                           right_it == right_chunks_by_group.end()
                                               ? std::vector<const ObjectValue*>{}
                                               : local_rows(right_it->second),
                                           left_columns,
                                           right_columns,
                                           spec);
        if (!chunk.rows.empty()) {
            chunks.push_back(std::move(chunk));
        }
    }
    if (spec.method == plan::JoinMethod::Right || spec.method == plan::JoinMethod::Full) {
        for (const auto& [group_key, right_chunks] : right_chunks_by_group) {
            if (processed_groups.count(group_key) != 0) {
                continue;
            }
            auto chunk = local_hash_join_group(
                {}, local_rows(right_chunks), left_columns, right_columns, spec);
            if (!chunk.rows.empty()) {
                chunks.push_back(std::move(chunk));
            }
        }
    }

    Value value = Value::table_stream(
        left_table.bucket.empty() ? right_table.bucket : left_table.bucket,
        std::move(chunks),
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

absl::StatusOr<Page> collect_input_page(Operator* input) {
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
        return Page{};
    }
    return std::move(*output);
}

class ConnectorScanOperator final : public Operator {
public:
    explicit ConnectorScanOperator(optimizer::PushdownPlan plan) : plan_(std::move(plan)) {}

    [[nodiscard]] std::string name() const override { return "ConnectorScanOperator"; }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (out == nullptr) {
            return;
        }
        out->insert(out->end(), split_stats_.begin(), split_stats_.end());
        if (current_split_stats_.has_value()) {
            connector::ConnectorSplitStats current = *current_split_stats_;
            current.wall_time_ms = elapsed_ms(current_split_started_);
            out->push_back(current);
        }
    }

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
                current_split_stats_ = connector::ConnectorSplitStats{
                    .split_id = splits_[next_split_].split_id,
                    .metadata_time_ms = splits_[next_split_].metadata_time_ms,
                    .split_discovery_time_ms = splits_[next_split_].split_discovery_time_ms,
                };
                current_split_started_ = Clock::now();
                ++next_split_;
            }

            auto page_or = page_source_->NextPage();
            if (!page_or.ok()) {
                return page_or.status();
            }
            if (!page_or->has_value()) {
                if (next_split_ > 0) {
                    splits_[next_split_ - 1].finished = page_source_->Finished();
                    if (current_split_stats_.has_value()) {
                        auto& stats = current_split_stats_.value();
                        copy_source_profile(&stats, page_source_->Stats());
                        stats.finished = page_source_->Finished();
                        stats.wall_time_ms = elapsed_ms(current_split_started_);
                        split_stats_.push_back(stats);
                        current_split_stats_.reset();
                    }
                }
                page_source_.reset();
                continue;
            }
            auto status = ValidatePage(page_or->value());
            if (!status.ok()) {
                return status;
            }
            if (!current_split_stats_.has_value()) {
                return absl::InternalError("connector scan missing split stats");
            }
            auto& stats = current_split_stats_.value();
            copy_source_profile(&stats, page_source_->Stats());
            add_page_to_split_stats(&stats, page_or->value());
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
    std::optional<connector::ConnectorSplitStats> current_split_stats_;
    Clock::time_point current_split_started_;
    std::unique_ptr<connector::ConnectorPageSource> page_source_;
    size_t next_split_ = 0;
    bool initialized_ = false;
};

class ConnectorSplitScanOperator final : public Operator {
public:
    explicit ConnectorSplitScanOperator(connector::ConnectorSplit split)
        : split_(std::move(split)) {}

    [[nodiscard]] std::string name() const override { return "ConnectorScanOperator"; }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (out == nullptr) {
            return;
        }
        if (page_source_ != nullptr && current_split_stats_.has_value()) {
            connector::ConnectorSplitStats current = *current_split_stats_;
            current.wall_time_ms = elapsed_ms(current_split_started_);
            out->push_back(current);
        } else if (split_stats_.finished) {
            out->push_back(split_stats_);
        }
    }

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
            current_split_stats_ = connector::ConnectorSplitStats{
                .split_id = split_.split_id,
                .metadata_time_ms = split_.metadata_time_ms,
                .split_discovery_time_ms = split_.split_discovery_time_ms,
            };
            current_split_started_ = Clock::now();
            initialized_ = true;
        }
        auto page_or = page_source_->NextPage();
        if (!page_or.ok()) {
            return page_or.status();
        }
        if (!page_or->has_value()) {
            split_.finished = page_source_->Finished();
            if (current_split_stats_.has_value()) {
                auto& stats = current_split_stats_.value();
                copy_source_profile(&stats, page_source_->Stats());
                stats.finished = page_source_->Finished();
                stats.wall_time_ms = elapsed_ms(current_split_started_);
                split_stats_ = stats;
                current_split_stats_.reset();
            }
            return std::nullopt;
        }
        auto status = ValidatePage(page_or->value());
        if (!status.ok()) {
            return status;
        }
        if (!current_split_stats_.has_value()) {
            return absl::InternalError("connector split scan missing split stats");
        }
        auto& stats = current_split_stats_.value();
        copy_source_profile(&stats, page_source_->Stats());
        add_page_to_split_stats(&stats, page_or->value());
        return page_or;
    }

private:
    connector::ConnectorSplit split_;
    connector::ConnectorSplitStats split_stats_;
    std::optional<connector::ConnectorSplitStats> current_split_stats_;
    Clock::time_point current_split_started_;
    std::unique_ptr<connector::ConnectorRuntime> runtime_;
    std::unique_ptr<connector::ConnectorPageSource> page_source_;
    bool initialized_ = false;
};

class ExchangeBuffer {
public:
    explicit ExchangeBuffer(size_t max_pages = 1024, size_t max_buffered_bytes = 64 * 1024 * 1024)
        : max_pages_(std::max<size_t>(1, max_pages)),
          max_buffered_bytes_(std::max<size_t>(1, max_buffered_bytes)) {}

    void SetProducerCount(size_t producer_count) {
        std::scoped_lock lock(mu_);
        producer_count_ = std::max<size_t>(1, producer_count);
    }

    absl::Status AddPage(Page page) {
        const size_t page_bytes = EstimatePageBytes(page);
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this, page_bytes]() {
            return closed_ || finished_ || error_.has_value() ||
                   (pages_.size() < max_pages_ &&
                    (pages_.empty() || buffered_bytes_ + page_bytes <= max_buffered_bytes_));
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
        buffered_bytes_ += page_bytes;
        pages_.push_back(std::move(page));
        lock.unlock();
        cv_.notify_all();
        return absl::OkStatus();
    }

    absl::StatusOr<std::optional<Page>> PopPage() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() { return !pages_.empty() || finished_ || error_.has_value(); });
        if (error_.has_value()) {
            return absl::FailedPreconditionError(*error_);
        }
        if (pages_.empty()) {
            return std::nullopt;
        }
        Page page = std::move(pages_.front());
        pages_.pop_front();
        const size_t page_bytes = EstimatePageBytes(page);
        buffered_bytes_ = page_bytes > buffered_bytes_ ? 0 : buffered_bytes_ - page_bytes;
        lock.unlock();
        cv_.notify_all();
        return page;
    }

    [[nodiscard]] size_t page_count() const {
        std::scoped_lock lock(mu_);
        return pages_.size();
    }
    [[nodiscard]] size_t row_count() const {
        std::scoped_lock lock(mu_);
        return rows_;
    }
    [[nodiscard]] bool finished() const {
        std::scoped_lock lock(mu_);
        return finished_;
    }
    [[nodiscard]] bool closed() const {
        std::scoped_lock lock(mu_);
        return closed_;
    }

    absl::Status Finish() {
        std::scoped_lock lock(mu_);
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
        std::scoped_lock lock(mu_);
        error_ = status.ToString();
        finished_ = true;
        cv_.notify_all();
    }

    void Close() {
        std::scoped_lock lock(mu_);
        closed_ = true;
        finished_ = true;
        cv_.notify_all();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Page> pages_;
    size_t max_pages_ = 1024;
    size_t max_buffered_bytes_ = size_t{64} * 1024 * 1024;
    size_t buffered_bytes_ = 0;
    size_t producer_count_ = 1;
    size_t finished_producers_ = 0;
    size_t rows_ = 0;
    bool finished_ = false;
    bool closed_ = false;
    std::optional<std::string> error_;
};

ExchangeDistributionProfile GatherDistribution(size_t partitions = 1) {
    return ExchangeDistributionProfile{
        .kind = "gather",
        .partition_keys = {},
        .include_group_key = false,
        .partitions = partitions,
    };
}

ExchangeDistributionProfile HashDistribution(std::vector<std::string> partition_keys,
                                             bool include_group_key,
                                             size_t partitions) {
    return ExchangeDistributionProfile{
        .kind = "hash",
        .partition_keys = std::move(partition_keys),
        .include_group_key = include_group_key,
        .partitions = partitions,
    };
}

ExchangeDistributionProfile RoundRobinDistribution(size_t partitions) {
    return ExchangeDistributionProfile{
        .kind = "round_robin",
        .partition_keys = {},
        .include_group_key = false,
        .partitions = partitions,
    };
}

ExchangeDistributionProfile BroadcastDistribution(size_t partitions) {
    return ExchangeDistributionProfile{
        .kind = "broadcast",
        .partition_keys = {},
        .include_group_key = false,
        .partitions = partitions,
    };
}

class ExchangeSinkOperator final : public Operator {
public:
    ExchangeSinkOperator(std::unique_ptr<Operator> input, std::shared_ptr<ExchangeBuffer> buffer)
        : input_(std::move(input)), buffer_(std::move(buffer)) {}

    [[nodiscard]] std::string name() const override { return "ExchangeSinkOperator"; }

    void Cancel() override {
        if (buffer_ != nullptr) {
            buffer_->Close();
        }
        if (input_ != nullptr) {
            input_->Cancel();
        }
    }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectSplitStats(out);
        }
    }

    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectAccumulatorStats(out);
        }
    }

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

size_t partition_index_for_key(const std::string& key, size_t partition_count) {
    if (partition_count == 0) {
        return 0;
    }
    return std::hash<std::string>{}(key) % partition_count;
}

std::optional<std::string> partition_key_for_page_row(
    const PageChunk& chunk,
    size_t row_index,
    const std::vector<std::string>& partition_keys,
    bool include_group_key) {
    std::string key;
    if (chunk.group_key != nullptr) {
        if (include_group_key) {
            auto group_key = exchange_partition_key(*chunk.group_key, {});
            if (group_key.has_value()) {
                absl::StrAppend(&key, "group:\n", *group_key);
            }
        } else {
            return exchange_partition_key(*chunk.group_key, partition_keys);
        }
    }
    auto row = RowFromPageChunk(chunk, row_index);
    auto row_key = exchange_partition_key(*row, partition_keys);
    if (row_key.has_value()) {
        absl::StrAppend(&key, "row:\n", *row_key);
    }
    return key.empty() ? std::optional<std::string>("__single_partition__") : key;
}

class PartitionedExchangeSinkOperator final : public Operator {
public:
    PartitionedExchangeSinkOperator(std::unique_ptr<Operator> input,
                                    std::vector<std::shared_ptr<ExchangeBuffer>> buffers,
                                    ExchangeDistributionProfile distribution)
        : input_(std::move(input)),
          buffers_(std::move(buffers)),
          distribution_(std::move(distribution)) {
        partition_stats_.reserve(buffers_.size());
        for (size_t index = 0; index < buffers_.size(); ++index) {
            partition_stats_.push_back({.partition = index});
        }
    }

    [[nodiscard]] std::string name() const override {
        if (distribution_.kind == "broadcast") {
            return "BroadcastExchangeSinkOperator";
        }
        if (distribution_.kind == "round_robin") {
            return "RoundRobinPartitionExchangeSinkOperator";
        }
        return "HashPartitionExchangeSinkOperator";
    }

    void Cancel() override {
        for (const auto& buffer : buffers_) {
            if (buffer != nullptr) {
                buffer->Close();
            }
        }
        if (input_ != nullptr) {
            input_->Cancel();
        }
    }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectSplitStats(out);
        }
    }

    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectAccumulatorStats(out);
        }
    }

    void CollectExchangePartitionStats(std::vector<ExchangePartitionStats>* out) const override {
        if (out != nullptr) {
            out->insert(out->end(), partition_stats_.begin(), partition_stats_.end());
        }
    }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (finished_) {
            return std::nullopt;
        }
        if (buffers_.empty()) {
            return absl::InvalidArgumentError("partitioned exchange sink has no buffers");
        }
        auto page_or = next_input_page(input_.get());
        if (!page_or.ok()) {
            MarkBuffersError(page_or.status());
            return page_or.status();
        }
        if (!page_or->has_value()) {
            for (const auto& buffer : buffers_) {
                auto status = buffer->Finish();
                if (!status.ok()) {
                    return status;
                }
            }
            finished_ = true;
            return std::nullopt;
        }

        Page output = std::move(**page_or);
        auto status = ValidatePage(output);
        if (!status.ok()) {
            MarkBuffersError(status);
            return status;
        }

        std::vector<std::vector<TableChunk>> partitions(buffers_.size());
        for (const auto& chunk : output.chunks) {
            std::vector<std::string> columns;
            columns.reserve(chunk.columns.size());
            for (const auto& column : chunk.columns) {
                columns.push_back(column.name);
            }
            for (size_t row_index = 0; row_index < chunk.row_count; ++row_index) {
                auto row = RowFromPageChunk(chunk, row_index);
                auto append = [&](size_t partition) {
                    TableChunk partition_chunk;
                    partition_chunk.group_key = chunk.group_key;
                    partition_chunk.columns = columns;
                    partition_chunk.rows.push_back(row);
                    partitions[partition].push_back(std::move(partition_chunk));
                };
                if (distribution_.kind == "broadcast") {
                    for (size_t partition = 0; partition < buffers_.size(); ++partition) {
                        append(partition);
                    }
                    continue;
                }
                if (distribution_.kind == "round_robin") {
                    append(next_partition_++ % buffers_.size());
                    continue;
                }
                if (distribution_.kind != "hash") {
                    return absl::InvalidArgumentError(absl::StrCat(
                        "unsupported partitioned exchange distribution: ", distribution_.kind));
                }
                auto key = partition_key_for_page_row(chunk,
                                                      row_index,
                                                      distribution_.partition_keys,
                                                      distribution_.include_group_key);
                if (key.has_value()) {
                    append(partition_index_for_key(*key, buffers_.size()));
                }
            }
        }

        for (size_t index = 0; index < partitions.size(); ++index) {
            if (partitions[index].empty()) {
                continue;
            }
            Page partitioned = PageFromTableChunks(output.bucket,
                                                   partitions[index],
                                                   output.range_start,
                                                   output.range_stop,
                                                   output.result_name);
            partitioned.plan = output.plan;
            partitioned.materialized = output.materialized;
            partition_stats_[index].rows += partitioned.row_count();
            partition_stats_[index].bytes += EstimatePageBytes(partitioned);
            auto add_status = buffers_[index]->AddPage(std::move(partitioned));
            if (!add_status.ok()) {
                return add_status;
            }
        }
        return output;
    }

private:
    void MarkBuffersError(const absl::Status& status) {
        for (const auto& buffer : buffers_) {
            if (buffer != nullptr) {
                buffer->MarkError(status);
            }
        }
    }

    std::unique_ptr<Operator> input_;
    std::vector<std::shared_ptr<ExchangeBuffer>> buffers_;
    ExchangeDistributionProfile distribution_;
    std::vector<ExchangePartitionStats> partition_stats_;
    size_t next_partition_ = 0;
    bool finished_ = false;
};

class ExchangeSourceOperator final : public Operator {
public:
    explicit ExchangeSourceOperator(std::shared_ptr<ExchangeBuffer> buffer)
        : buffer_(std::move(buffer)) {}

    [[nodiscard]] std::string name() const override { return "ExchangeSourceOperator"; }

    void Cancel() override {
        if (buffer_ != nullptr) {
            buffer_->Close();
        }
    }

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

    void Cancel() override {
        if (input_ != nullptr) {
            input_->Cancel();
        }
    }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectSplitStats(out);
        }
    }

    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectAccumulatorStats(out);
        }
    }

protected:
    [[nodiscard]] const std::shared_ptr<plan::PlanNode>& plan() const { return plan_; }
    [[nodiscard]] Operator* input() const { return input_.get(); }
    virtual absl::StatusOr<Page> Apply(Page input) const = 0;

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
};

class BlockingPageUnaryOperator : public Operator {
public:
    BlockingPageUnaryOperator(std::shared_ptr<plan::PlanNode> plan, std::unique_ptr<Operator> input)
        : plan_(std::move(plan)), input_(std::move(input)) {}

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (emitted_) {
            return std::nullopt;
        }
        emitted_ = true;
        auto input_or = collect_input_page(input_.get());
        if (!input_or.ok()) {
            return input_or.status();
        }
        auto page_or = Apply(std::move(*input_or));
        if (!page_or.ok()) {
            return page_or.status();
        }
        return std::move(*page_or);
    }

    void Cancel() override {
        if (input_ != nullptr) {
            input_->Cancel();
        }
    }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectSplitStats(out);
        }
    }

    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectAccumulatorStats(out);
        }
    }

protected:
    [[nodiscard]] const std::shared_ptr<plan::PlanNode>& plan() const { return plan_; }
    virtual absl::StatusOr<Page> Apply(Page input) const = 0;

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
        return apply_range_page(input, plan());
    }
};

class FilterOperator final : public PageUnaryOperator {
public:
    using PageUnaryOperator::PageUnaryOperator;
    [[nodiscard]] std::string name() const override { return "FilterOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override {
        return apply_filter_page(input, plan());
    }
};

class ProjectOperator final : public PageUnaryOperator {
public:
    using PageUnaryOperator::PageUnaryOperator;
    [[nodiscard]] std::string name() const override { return "ProjectOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override {
        return apply_project_page(input, plan());
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

    void Cancel() override {
        if (input_ != nullptr) {
            input_->Cancel();
        }
    }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectSplitStats(out);
        }
    }

    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectAccumulatorStats(out);
        }
    }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        auto input_or = next_input_page(input_.get());
        if (!input_or.ok()) {
            return input_or.status();
        }
        if (!input_or->has_value()) {
            return std::nullopt;
        }
        auto page_or = apply_exchange_page(input_or->value(), plan_);
        if (!page_or.ok()) {
            return page_or.status();
        }
        return std::move(*page_or);
    }

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
};

class LimitOperator final : public Operator {
public:
    LimitOperator(std::shared_ptr<plan::PlanNode> plan, std::unique_ptr<Operator> input)
        : plan_(std::move(plan)),
          input_(std::move(input)),
          remaining_offset_(std::max<int64_t>(0, plan_->limit().offset)),
          remaining_limit_(std::max<int64_t>(0, plan_->limit().n)) {}
    [[nodiscard]] std::string name() const override { return "LimitOperator"; }

    void Cancel() override {
        if (input_ != nullptr) {
            input_->Cancel();
        }
    }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectSplitStats(out);
        }
    }

    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectAccumulatorStats(out);
        }
    }

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
                if (std::cmp_greater_equal(remaining_offset_, source.row_count)) {
                    remaining_offset_ -= static_cast<int64_t>(source.row_count);
                    continue;
                }
                const auto start = static_cast<size_t>(remaining_offset_);
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

class SortOperator final : public BlockingPageUnaryOperator {
public:
    using BlockingPageUnaryOperator::BlockingPageUnaryOperator;
    [[nodiscard]] std::string name() const override { return "SortOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override { return apply_sort_page(input, plan()); }
};

class TopNOperator final : public BlockingPageUnaryOperator {
public:
    TopNOperator(std::shared_ptr<plan::PlanNode> sort_plan,
                 size_t limit,
                 std::unique_ptr<Operator> input)
        : BlockingPageUnaryOperator(std::move(sort_plan), std::move(input)), limit_(limit) {}

    [[nodiscard]] std::string name() const override { return "TopNOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override {
        return apply_topn_page(input, plan(), limit_);
    }

    size_t limit_ = 0;
};

class LocalHashJoinOperator final : public Operator {
public:
    LocalHashJoinOperator(std::shared_ptr<plan::PlanNode> plan,
                          std::unique_ptr<Operator> left,
                          std::unique_ptr<Operator> right)
        : plan_(std::move(plan)), left_(std::move(left)), right_(std::move(right)) {}

    [[nodiscard]] std::string name() const override { return "LocalHashJoinOperator"; }

    void Cancel() override {
        if (left_ != nullptr) {
            left_->Cancel();
        }
        if (right_ != nullptr) {
            right_->Cancel();
        }
    }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (left_ != nullptr) {
            left_->CollectSplitStats(out);
        }
        if (right_ != nullptr) {
            right_->CollectSplitStats(out);
        }
    }

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

    void Cancel() override {
        if (input_ != nullptr) {
            input_->Cancel();
        }
    }

    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectSplitStats(out);
        }
    }

    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override {
        if (input_ != nullptr) {
            input_->CollectAccumulatorStats(out);
        }
    }

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

bool IsPipelineBreakerKind(plan::PlanNodeKind kind) {
    return kind == plan::PlanNodeKind::Sort || kind == plan::PlanNodeKind::Group ||
           kind == plan::PlanNodeKind::Aggregate || kind == plan::PlanNodeKind::Distinct ||
           kind == plan::PlanNodeKind::Exchange || kind == plan::PlanNodeKind::Materialize;
}

absl::StatusOr<std::unique_ptr<Operator>> WrapUnaryOperator(
    const std::shared_ptr<plan::PlanNode>& node,
    std::unique_ptr<Operator> input,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (node == nullptr) {
        return absl::InvalidArgumentError("missing unary operator plan");
    }
    if (node->kind == plan::PlanNodeKind::Materialize) {
        return std::unique_ptr<Operator>(new MaterializeOperator(node, std::move(input)));
    }
    if (node->kind == plan::PlanNodeKind::Range) {
        return std::unique_ptr<Operator>(new RangeOperator(node, std::move(input)));
    }
    if (node->kind == plan::PlanNodeKind::Filter) {
        return std::unique_ptr<Operator>(new FilterOperator(node, std::move(input)));
    }
    if (node->kind == plan::PlanNodeKind::Project) {
        return std::unique_ptr<Operator>(new ProjectOperator(node, std::move(input)));
    }
    if (node->kind == plan::PlanNodeKind::Rename) {
        return std::unique_ptr<Operator>(new RenameOperator(node, std::move(input)));
    }
    if (node->kind == plan::PlanNodeKind::Exchange) {
        return std::unique_ptr<Operator>(new ExchangeOperator(node, std::move(input)));
    }
    if (node->kind == plan::PlanNodeKind::Limit) {
        return std::unique_ptr<Operator>(new LimitOperator(node, std::move(input)));
    }
    if (node->kind == plan::PlanNodeKind::Sort) {
        return std::unique_ptr<Operator>(new SortOperator(node, std::move(input)));
    }
    if (node->kind == plan::PlanNodeKind::Group) {
        return std::unique_ptr<Operator>(
            new StreamingGroupOperator(node, std::move(input), memory_context));
    }
    if (node->kind == plan::PlanNodeKind::Distinct) {
        return std::unique_ptr<Operator>(
            new StreamingDistinctOperator(node, std::move(input), memory_context));
    }
    if (node->kind == plan::PlanNodeKind::Aggregate) {
        return std::unique_ptr<Operator>(
            new StreamingAggregateOperator(node, std::move(input), memory_context));
    }
    return absl::InvalidArgumentError(
        absl::StrCat("unsupported physical operator: ", plan::PlanNodeKindName(node->kind)));
}

struct PlannedOperator {
    std::unique_ptr<Operator> root;
    std::vector<std::unique_ptr<Operator>> driver_roots;
    std::vector<std::string> operators;
};

absl::StatusOr<PlannedOperator> BuildOperator(
    const std::shared_ptr<plan::PlanNode>& logical_plan,
    bool allow_multi_driver,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
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
                planned.operators.emplace_back("ConnectorScanOperator");
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
        auto left_or = BuildOperator(optimized_plan->inputs[0], false, memory_context);
        if (!left_or.ok()) {
            return left_or.status();
        }
        auto right_or = BuildOperator(optimized_plan->inputs[1], false, memory_context);
        if (!right_or.ok()) {
            return right_or.status();
        }
        PlannedOperator planned;
        planned.operators = std::move(left_or->operators);
        planned.operators.insert(
            planned.operators.end(), right_or->operators.begin(), right_or->operators.end());
        planned.root = std::unique_ptr<Operator>(new LocalHashJoinOperator(
            optimized_plan, std::move(left_or->root), std::move(right_or->root)));
        planned.operators.push_back(planned.root->name());
        return planned;
    }
    if (optimized_plan->kind == plan::PlanNodeKind::Aggregate &&
        optimized_plan->inputs.size() == 1 && optimized_plan->inputs[0] != nullptr &&
        optimized_plan->inputs[0]->kind == plan::PlanNodeKind::Group &&
        optimized_plan->inputs[0]->inputs.size() == 1 &&
        optimized_plan->inputs[0]->inputs[0] != nullptr) {
        auto planned_or =
            BuildOperator(optimized_plan->inputs[0]->inputs[0], false, memory_context);
        if (!planned_or.ok()) {
            return planned_or.status();
        }
        planned_or->root = std::unique_ptr<Operator>(
            new StreamingGroupedAggregateOperator(optimized_plan,
                                                  optimized_plan->inputs[0]->group().columns,
                                                  std::move(planned_or->root),
                                                  memory_context));
        planned_or->operators.emplace_back("GroupOperator");
        planned_or->operators.push_back(planned_or->root->name());
        return std::move(*planned_or);
    }
    if (optimized_plan->inputs.size() != 1 || optimized_plan->inputs[0] == nullptr) {
        return absl::InvalidArgumentError("plan is not executable");
    }
    auto planned_or = BuildOperator(optimized_plan->inputs[0], false, memory_context);
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
        current = std::unique_ptr<Operator>(new StreamingGroupOperator(
            optimized_plan, std::move(planned_or->root), memory_context));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Distinct) {
        current = std::unique_ptr<Operator>(new StreamingDistinctOperator(
            optimized_plan, std::move(planned_or->root), memory_context));
    } else if (optimized_plan->kind == plan::PlanNodeKind::Aggregate) {
        current = std::unique_ptr<Operator>(new StreamingAggregateOperator(
            optimized_plan, std::move(planned_or->root), memory_context));
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
                      std::vector<std::unique_ptr<Operator>> driver_roots = {},
                      std::optional<ExchangeDistributionProfile> distribution = std::nullopt) {
    Pipeline pipeline;
    pipeline.id = std::move(id);
    pipeline.name = std::move(name);
    pipeline.role = std::move(role);
    pipeline.dependencies = std::move(dependencies);
    pipeline.operators = std::move(operators);
    pipeline.distribution = std::move(distribution);
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

enum class GroupedAggregateStrategy {
    SingleStage,
    TwoStageGather,
    TwoStagePartitioned,
};

std::optional<double> EstimateRowsForPlan(const std::shared_ptr<plan::PlanNode>& plan_node) {
    auto cbo_or = optimizer::DefaultCostBasedOptimizer().OptimizeWithTrace(plan_node);
    if (!cbo_or.ok()) {
        return std::nullopt;
    }
    return cbo_or->cost.rows;
}

GroupedAggregateStrategy ChooseGroupedAggregateStrategy(
    const std::shared_ptr<plan::PlanNode>& root_plan,
    size_t aggregate_index,
    const std::shared_ptr<plan::PlanNode>& group,
    const std::shared_ptr<plan::PlanNode>& input_plan,
    size_t driver_count) {
    constexpr double kSmallInputRows = 4096.0;
    constexpr double kPartitionedGroupRows = 128.0;
    if (driver_count <= 1 || group == nullptr || group->group().columns.empty()) {
        return GroupedAggregateStrategy::SingleStage;
    }

    const auto input_rows = EstimateRowsForPlan(input_plan);
    if (input_rows.has_value() && *input_rows < kSmallInputRows) {
        return GroupedAggregateStrategy::SingleStage;
    }

    const auto grouped_rows = EstimateRowsForPlan(group);
    const bool aggregate_is_root = root_plan != nullptr && aggregate_index == 0;
    if (aggregate_is_root && grouped_rows.has_value() && *grouped_rows >= kPartitionedGroupRows) {
        return GroupedAggregateStrategy::TwoStagePartitioned;
    }
    if (aggregate_is_root && !grouped_rows.has_value() && driver_count >= 4) {
        return GroupedAggregateStrategy::TwoStagePartitioned;
    }
    return GroupedAggregateStrategy::TwoStageGather;
}

struct ProducedPipeline {
    std::shared_ptr<ExchangeBuffer> buffer;
    std::string pipeline_id;
};

struct PartitionedProducedPipeline {
    std::vector<std::shared_ptr<ExchangeBuffer>> buffers;
    std::string pipeline_id;
};

struct UnaryWrappedJoinPlan {
    std::shared_ptr<plan::PlanNode> join;
    std::vector<std::shared_ptr<plan::PlanNode>> wrappers;
};

std::optional<UnaryWrappedJoinPlan> FindUnaryWrappedJoinPlan(
    const std::shared_ptr<plan::PlanNode>& root) {
    UnaryWrappedJoinPlan result;
    auto cursor = root;
    while (cursor != nullptr && cursor->kind != plan::PlanNodeKind::Join) {
        if (cursor->inputs.size() != 1 || cursor->inputs[0] == nullptr) {
            return std::nullopt;
        }
        result.wrappers.push_back(cursor);
        cursor = cursor->inputs[0];
    }
    if (cursor == nullptr || cursor->inputs.size() != 2 || cursor->inputs[0] == nullptr ||
        cursor->inputs[1] == nullptr) {
        return std::nullopt;
    }
    result.join = std::move(cursor);
    return result;
}

bool CanPartitionUnaryWrappedJoin(const UnaryWrappedJoinPlan& plan) {
    for (const auto& wrapper : plan.wrappers) {
        if (wrapper == nullptr) {
            return false;
        }
        switch (wrapper->kind) {
            case plan::PlanNodeKind::Range:
            case plan::PlanNodeKind::Filter:
            case plan::PlanNodeKind::Project:
            case plan::PlanNodeKind::Rename:
            case plan::PlanNodeKind::Materialize:
                break;
            default:
                return false;
        }
    }
    return true;
}

struct LocalHashJoinPartitionStrategy {
    size_t partitions = 1;
    ExchangeDistributionProfile left;
    ExchangeDistributionProfile right;
    ExchangeDistributionProfile output;
};

LocalHashJoinPartitionStrategy ChooseLocalHashJoinPartitionStrategy(
    const UnaryWrappedJoinPlan& wrapped_join) {
    constexpr double kMinPartitionedRows = 8192.0;
    constexpr double kTargetRowsPerPartition = 4096.0;
    constexpr double kMaxBroadcastBuildRows = 1024.0;
    constexpr double kMinBroadcastProbeRows = 8192.0;
    constexpr double kMinBroadcastProbeToBuildRatio = 8.0;
    constexpr size_t kMaxPartitions = 8;
    const auto& join = wrapped_join.join;
    if (!CanPartitionUnaryWrappedJoin(wrapped_join) || join == nullptr ||
        join->kind != plan::PlanNodeKind::Join || join->inputs.size() != 2 ||
        join->inputs[0] == nullptr || join->inputs[1] == nullptr || join->join().on.empty()) {
        return {};
    }
    const auto left_rows = EstimateRowsForPlan(join->inputs[0]);
    const auto right_rows = EstimateRowsForPlan(join->inputs[1]);
    if (!left_rows.has_value() || !right_rows.has_value()) {
        return {};
    }
    const double input_rows = *left_rows + *right_rows;
    if (input_rows < kMinPartitionedRows) {
        return {};
    }
    const size_t partitions = std::min(
        kMaxPartitions,
        std::max<size_t>(2, static_cast<size_t>(std::ceil(input_rows / kTargetRowsPerPartition))));
    const bool build_left = join->join().build_side == plan::JoinBuildSide::Left;
    const double build_rows = build_left ? *left_rows : *right_rows;
    const double probe_rows = build_left ? *right_rows : *left_rows;
    if (join->join().method == plan::JoinMethod::Inner && build_rows > 0.0 &&
        build_rows <= kMaxBroadcastBuildRows && probe_rows >= kMinBroadcastProbeRows &&
        probe_rows >= build_rows * kMinBroadcastProbeToBuildRatio) {
        const auto broadcast = BroadcastDistribution(partitions);
        const auto round_robin = RoundRobinDistribution(partitions);
        return LocalHashJoinPartitionStrategy{
            .partitions = partitions,
            .left = build_left ? broadcast : round_robin,
            .right = build_left ? round_robin : broadcast,
            .output = round_robin,
        };
    }
    const auto hash = HashDistribution(join->join().on, true, partitions);
    return LocalHashJoinPartitionStrategy{
        .partitions = partitions,
        .left = hash,
        .right = hash,
        .output = hash,
    };
}

absl::StatusOr<std::pair<std::unique_ptr<Operator>, std::vector<std::string>>>
BuildUnaryWrappedJoinOperator(const UnaryWrappedJoinPlan& plan,
                              std::unique_ptr<Operator> left,
                              std::unique_ptr<Operator> right,
                              const std::shared_ptr<QueryMemoryContext>& memory_context) {
    std::vector<std::string> operators = {left->name(), right->name()};
    std::unique_ptr<Operator> current(
        new LocalHashJoinOperator(plan.join, std::move(left), std::move(right)));
    operators.push_back(current->name());
    for (const auto& wrapper : std::views::reverse(plan.wrappers)) {
        auto wrapped_or = WrapUnaryOperator(wrapper, std::move(current), memory_context);
        if (!wrapped_or.ok()) {
            return wrapped_or.status();
        }
        current = std::move(*wrapped_or);
        operators.push_back(current->name());
    }
    return std::pair<std::unique_ptr<Operator>, std::vector<std::string>>{std::move(current),
                                                                          std::move(operators)};
}

absl::StatusOr<PlannedOperator> BuildPartitionedUnaryWrappedJoinOperators(
    const UnaryWrappedJoinPlan& plan,
    const std::vector<std::shared_ptr<ExchangeBuffer>>& left_buffers,
    const std::vector<std::shared_ptr<ExchangeBuffer>>& right_buffers,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (left_buffers.empty() || left_buffers.size() != right_buffers.size()) {
        return absl::InvalidArgumentError("partitioned join requires matching input buffers");
    }
    PlannedOperator planned;
    planned.driver_roots.reserve(left_buffers.size());
    for (size_t index = 0; index < left_buffers.size(); ++index) {
        auto operator_or = BuildUnaryWrappedJoinOperator(
            plan,
            std::unique_ptr<Operator>(new ExchangeSourceOperator(left_buffers[index])),
            std::unique_ptr<Operator>(new ExchangeSourceOperator(right_buffers[index])),
            memory_context);
        if (!operator_or.ok()) {
            return operator_or.status();
        }
        auto [root, operators] = std::move(*operator_or);
        if (planned.operators.empty()) {
            planned.operators = std::move(operators);
        }
        planned.driver_roots.push_back(std::move(root));
    }
    return planned;
}

std::string ChildPipelineId(const std::string& parent, std::string suffix) {
    return parent.empty() ? std::move(suffix) : absl::StrCat(parent, "-", suffix);
}

absl::StatusOr<ProducedPipeline> AddProducerPipelineForPlan(
    const std::shared_ptr<plan::PlanNode>& plan_node,
    std::string id,
    std::string role,
    ExecutionTask* task,
    const std::shared_ptr<QueryMemoryContext>& memory_context);

std::vector<std::shared_ptr<ExchangeBuffer>> MakePartitionedExchangeBuffers(size_t partition_count,
                                                                            size_t producer_count) {
    std::vector<std::shared_ptr<ExchangeBuffer>> buffers;
    buffers.reserve(partition_count);
    for (size_t index = 0; index < partition_count; ++index) {
        auto buffer = std::make_shared<ExchangeBuffer>();
        buffer->SetProducerCount(producer_count);
        buffers.push_back(std::move(buffer));
    }
    return buffers;
}

absl::StatusOr<PartitionedProducedPipeline> AddPartitionedProducerPipelineForPlan(
    const std::shared_ptr<plan::PlanNode>& plan_node,
    std::string id,
    std::string role,
    ExchangeDistributionProfile distribution,
    ExecutionTask* task,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (task == nullptr) {
        return absl::InvalidArgumentError("missing execution task");
    }
    if (distribution.partitions <= 1) {
        return absl::InvalidArgumentError("partitioned producer requires multiple partitions");
    }
    if (FindUnaryWrappedJoinPlan(plan_node).has_value()) {
        auto input_or = AddProducerPipelineForPlan(
            plan_node, ChildPipelineId(id, "source"), role, task, memory_context);
        if (!input_or.ok()) {
            return input_or.status();
        }
        auto buffers = MakePartitionedExchangeBuffers(distribution.partitions, 1);
        std::unique_ptr<Operator> source(new ExchangeSourceOperator(input_or->buffer));
        std::vector<std::string> operators = {source->name()};
        std::unique_ptr<Operator> sink(
            new PartitionedExchangeSinkOperator(std::move(source), buffers, distribution));
        operators.push_back(sink->name());
        task->pipelines.push_back(MakePipeline(std::move(id),
                                               "repartition producer",
                                               std::move(role),
                                               {input_or->pipeline_id},
                                               std::move(operators),
                                               std::move(sink),
                                               {},
                                               distribution));
        return PartitionedProducedPipeline{.buffers = std::move(buffers),
                                           .pipeline_id = task->pipelines.back().id};
    }

    auto planned_or = BuildOperator(plan_node, true, memory_context);
    if (!planned_or.ok()) {
        return planned_or.status();
    }
    std::vector<std::unique_ptr<Operator>> roots;
    if (!planned_or->driver_roots.empty()) {
        roots = std::move(planned_or->driver_roots);
    } else if (planned_or->root != nullptr) {
        roots.push_back(std::move(planned_or->root));
    } else {
        return absl::InvalidArgumentError("partitioned producer has no operator");
    }
    auto buffers = MakePartitionedExchangeBuffers(distribution.partitions, roots.size());
    std::vector<std::unique_ptr<Operator>> sinks;
    sinks.reserve(roots.size());
    for (auto& root : roots) {
        sinks.push_back(std::unique_ptr<Operator>(
            new PartitionedExchangeSinkOperator(std::move(root), buffers, distribution)));
    }
    auto operators = std::move(planned_or->operators);
    operators.push_back(sinks.front()->name());
    task->pipelines.push_back(MakePipeline(std::move(id),
                                           "partitioned producer",
                                           std::move(role),
                                           {},
                                           std::move(operators),
                                           nullptr,
                                           std::move(sinks),
                                           distribution));
    return PartitionedProducedPipeline{.buffers = std::move(buffers),
                                       .pipeline_id = task->pipelines.back().id};
}

struct ParallelInputPlan {
    PlannedOperator planned;
    std::vector<std::string> dependencies{};
};

absl::StatusOr<ParallelInputPlan> BuildParallelInputForPlan(
    const std::shared_ptr<plan::PlanNode>& plan_node,
    const std::string& id,
    ExecutionTask* task,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (task == nullptr) {
        return absl::InvalidArgumentError("missing execution task");
    }
    auto wrapped_join = FindUnaryWrappedJoinPlan(plan_node);
    if (wrapped_join.has_value()) {
        const auto& join = wrapped_join->join;
        const auto strategy = ChooseLocalHashJoinPartitionStrategy(*wrapped_join);
        if (strategy.partitions > 1) {
            const bool build_left = join->join().build_side == plan::JoinBuildSide::Left;
            auto left_or = AddPartitionedProducerPipelineForPlan(join->inputs[0],
                                                                 ChildPipelineId(id, "left"),
                                                                 build_left ? "build" : "probe",
                                                                 strategy.left,
                                                                 task,
                                                                 memory_context);
            if (!left_or.ok()) {
                return left_or.status();
            }
            auto right_or = AddPartitionedProducerPipelineForPlan(join->inputs[1],
                                                                  ChildPipelineId(id, "right"),
                                                                  build_left ? "probe" : "build",
                                                                  strategy.right,
                                                                  task,
                                                                  memory_context);
            if (!right_or.ok()) {
                return right_or.status();
            }
            auto planned_or = BuildPartitionedUnaryWrappedJoinOperators(
                *wrapped_join, left_or->buffers, right_or->buffers, memory_context);
            if (!planned_or.ok()) {
                return planned_or.status();
            }
            return ParallelInputPlan{
                .planned = std::move(*planned_or),
                .dependencies = {left_or->pipeline_id, right_or->pipeline_id},
            };
        }
    }

    auto planned_or = BuildOperator(plan_node, true, memory_context);
    if (!planned_or.ok()) {
        return planned_or.status();
    }
    return ParallelInputPlan{.planned = std::move(*planned_or)};
}

absl::StatusOr<ProducedPipeline> AddUnaryWrappedJoinProducerPipeline(
    const UnaryWrappedJoinPlan& wrapped_join,
    std::string id,
    std::string role,
    ExecutionTask* task) {
    const auto& join_plan = wrapped_join.join;
    if (join_plan == nullptr || join_plan->kind != plan::PlanNodeKind::Join ||
        join_plan->inputs.size() != 2 || join_plan->inputs[0] == nullptr ||
        join_plan->inputs[1] == nullptr) {
        return absl::InvalidArgumentError("join execution task requires two inputs");
    }

    const bool build_left = join_plan->join().build_side == plan::JoinBuildSide::Left;
    const auto strategy = ChooseLocalHashJoinPartitionStrategy(wrapped_join);
    if (strategy.partitions > 1) {
        auto left_or = AddPartitionedProducerPipelineForPlan(join_plan->inputs[0],
                                                             ChildPipelineId(id, "left"),
                                                             build_left ? "build" : "probe",
                                                             strategy.left,
                                                             task,
                                                             task->memory_context);
        if (!left_or.ok()) {
            return left_or.status();
        }
        auto right_or = AddPartitionedProducerPipelineForPlan(join_plan->inputs[1],
                                                              ChildPipelineId(id, "right"),
                                                              build_left ? "probe" : "build",
                                                              strategy.right,
                                                              task,
                                                              task->memory_context);
        if (!right_or.ok()) {
            return right_or.status();
        }
        auto planned_or = BuildPartitionedUnaryWrappedJoinOperators(
            wrapped_join, left_or->buffers, right_or->buffers, task->memory_context);
        if (!planned_or.ok()) {
            return planned_or.status();
        }
        auto output_buffer = std::make_shared<ExchangeBuffer>();
        output_buffer->SetProducerCount(planned_or->driver_roots.size());
        auto sinks =
            WrapDriverRootsWithExchangeSinks(std::move(planned_or->driver_roots), output_buffer);
        planned_or->operators.emplace_back("ExchangeSinkOperator");
        task->pipelines.push_back(MakePipeline(std::move(id),
                                               "partitioned join producer",
                                               std::move(role),
                                               {left_or->pipeline_id, right_or->pipeline_id},
                                               std::move(planned_or->operators),
                                               nullptr,
                                               std::move(sinks),
                                               strategy.output));
        return ProducedPipeline{.buffer = std::move(output_buffer),
                                .pipeline_id = task->pipelines.back().id};
    }

    auto left_or = AddProducerPipelineForPlan(join_plan->inputs[0],
                                              ChildPipelineId(id, "left"),
                                              build_left ? "build" : "probe",
                                              task,
                                              task->memory_context);
    if (!left_or.ok()) {
        return left_or.status();
    }
    auto right_or = AddProducerPipelineForPlan(join_plan->inputs[1],
                                               ChildPipelineId(id, "right"),
                                               build_left ? "probe" : "build",
                                               task,
                                               task->memory_context);
    if (!right_or.ok()) {
        return right_or.status();
    }

    auto output_buffer = std::make_shared<ExchangeBuffer>();
    auto operator_or = BuildUnaryWrappedJoinOperator(
        wrapped_join,
        std::unique_ptr<Operator>(new ExchangeSourceOperator(left_or->buffer)),
        std::unique_ptr<Operator>(new ExchangeSourceOperator(right_or->buffer)),
        task->memory_context);
    if (!operator_or.ok()) {
        return operator_or.status();
    }
    auto [join, root_operators] = std::move(*operator_or);
    std::unique_ptr<Operator> sink(new ExchangeSinkOperator(std::move(join), output_buffer));
    root_operators.push_back(sink->name());

    task->pipelines.push_back(MakePipeline(std::move(id),
                                           "join producer",
                                           std::move(role),
                                           {left_or->pipeline_id, right_or->pipeline_id},
                                           std::move(root_operators),
                                           std::move(sink),
                                           {},
                                           GatherDistribution()));
    return ProducedPipeline{.buffer = std::move(output_buffer),
                            .pipeline_id = task->pipelines.back().id};
}

absl::StatusOr<ProducedPipeline> AddProducerPipelineForPlan(
    const std::shared_ptr<plan::PlanNode>& plan_node,
    std::string id,
    std::string role,
    ExecutionTask* task,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (task == nullptr) {
        return absl::InvalidArgumentError("missing execution task");
    }
    if (auto join = FindUnaryWrappedJoinPlan(plan_node); join.has_value()) {
        return AddUnaryWrappedJoinProducerPipeline(*join, std::move(id), std::move(role), task);
    }

    auto planned_or = BuildOperator(plan_node, true, memory_context);
    if (!planned_or.ok()) {
        return planned_or.status();
    }
    auto output_buffer = std::make_shared<ExchangeBuffer>();
    std::vector<std::string> operators = std::move(planned_or->operators);
    if (!planned_or->driver_roots.empty()) {
        output_buffer->SetProducerCount(planned_or->driver_roots.size());
        auto sinks =
            WrapDriverRootsWithExchangeSinks(std::move(planned_or->driver_roots), output_buffer);
        operators.emplace_back("ExchangeSinkOperator");
        task->pipelines.push_back(MakePipeline(std::move(id),
                                               "producer",
                                               std::move(role),
                                               {},
                                               std::move(operators),
                                               nullptr,
                                               std::move(sinks),
                                               GatherDistribution()));
        return ProducedPipeline{.buffer = std::move(output_buffer),
                                .pipeline_id = task->pipelines.back().id};
    }
    std::unique_ptr<Operator> sink(
        new ExchangeSinkOperator(std::move(planned_or->root), output_buffer));
    operators.push_back(sink->name());
    task->pipelines.push_back(MakePipeline(std::move(id),
                                           "producer",
                                           std::move(role),
                                           {},
                                           std::move(operators),
                                           std::move(sink),
                                           {},
                                           GatherDistribution()));
    return ProducedPipeline{.buffer = std::move(output_buffer),
                            .pipeline_id = task->pipelines.back().id};
}

absl::StatusOr<ExecutionTask> BuildUnaryWrappedJoinExecutionTask(
    const std::shared_ptr<plan::PlanNode>& root_plan,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    auto wrapped_join = FindUnaryWrappedJoinPlan(root_plan);
    if (!wrapped_join.has_value()) {
        return absl::InvalidArgumentError("join execution task requires a join input");
    }
    const auto& join_plan = wrapped_join->join;
    if (join_plan == nullptr || join_plan->kind != plan::PlanNodeKind::Join ||
        join_plan->inputs.size() != 2 || join_plan->inputs[0] == nullptr ||
        join_plan->inputs[1] == nullptr) {
        return absl::InvalidArgumentError("join execution task requires two inputs");
    }

    ExecutionTask task;
    task.memory_context = memory_context;
    const bool build_left = join_plan->join().build_side == plan::JoinBuildSide::Left;
    const auto strategy = ChooseLocalHashJoinPartitionStrategy(*wrapped_join);
    if (strategy.partitions > 1) {
        auto left_or = AddPartitionedProducerPipelineForPlan(join_plan->inputs[0],
                                                             "join-left",
                                                             build_left ? "build" : "probe",
                                                             strategy.left,
                                                             &task,
                                                             task.memory_context);
        if (!left_or.ok()) {
            return left_or.status();
        }
        auto right_or = AddPartitionedProducerPipelineForPlan(join_plan->inputs[1],
                                                              "join-right",
                                                              build_left ? "probe" : "build",
                                                              strategy.right,
                                                              &task,
                                                              task.memory_context);
        if (!right_or.ok()) {
            return right_or.status();
        }
        auto planned_or = BuildPartitionedUnaryWrappedJoinOperators(
            *wrapped_join, left_or->buffers, right_or->buffers, memory_context);
        if (!planned_or.ok()) {
            return planned_or.status();
        }
        planned_or->driver_roots = WrapDriverRootsWithOutput(std::move(planned_or->driver_roots));
        planned_or->operators.emplace_back("OutputOperator");
        task.pipelines.push_back(MakePipeline("main",
                                              "main partitioned join",
                                              "root",
                                              {left_or->pipeline_id, right_or->pipeline_id},
                                              std::move(planned_or->operators),
                                              nullptr,
                                              std::move(planned_or->driver_roots),
                                              strategy.output));
        return task;
    }

    auto left_or = AddProducerPipelineForPlan(join_plan->inputs[0],
                                              "join-left",
                                              build_left ? "build" : "probe",
                                              &task,
                                              task.memory_context);
    if (!left_or.ok()) {
        return left_or.status();
    }
    auto right_or = AddProducerPipelineForPlan(join_plan->inputs[1],
                                               "join-right",
                                               build_left ? "probe" : "build",
                                               &task,
                                               task.memory_context);
    if (!right_or.ok()) {
        return right_or.status();
    }

    auto operator_or = BuildUnaryWrappedJoinOperator(
        *wrapped_join,
        std::unique_ptr<Operator>(new ExchangeSourceOperator(left_or->buffer)),
        std::unique_ptr<Operator>(new ExchangeSourceOperator(right_or->buffer)),
        memory_context);
    if (!operator_or.ok()) {
        return operator_or.status();
    }
    auto [join, root_operators] = std::move(*operator_or);
    std::unique_ptr<Operator> output(new OutputOperator(std::move(join)));
    root_operators.push_back(output->name());

    task.pipelines.push_back(MakePipeline("main",
                                          "main",
                                          "root",
                                          {left_or->pipeline_id, right_or->pipeline_id},
                                          std::move(root_operators),
                                          std::move(output)));
    return task;
}

absl::StatusOr<ExecutionTask> BuildExchangeExecutionTask(
    const std::shared_ptr<plan::PlanNode>& exchange_plan,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (exchange_plan == nullptr || exchange_plan->kind != plan::PlanNodeKind::Exchange ||
        exchange_plan->inputs.size() != 1 || exchange_plan->inputs[0] == nullptr) {
        return absl::InvalidArgumentError("exchange execution task requires one input");
    }

    ExecutionTask task;
    task.memory_context = memory_context;
    auto input_or = AddProducerPipelineForPlan(
        exchange_plan->inputs[0], "exchange-input", "source", &task, task.memory_context);
    if (!input_or.ok()) {
        return input_or.status();
    }
    std::unique_ptr<Operator> source(new ExchangeSourceOperator(input_or->buffer));
    std::vector<std::string> operators = {source->name()};
    std::unique_ptr<Operator> exchange(new ExchangeOperator(exchange_plan, std::move(source)));
    operators.push_back(exchange->name());
    std::unique_ptr<Operator> output(new OutputOperator(std::move(exchange)));
    operators.push_back(output->name());
    task.pipelines.push_back(MakePipeline(
        "main", "main", "root", {input_or->pipeline_id}, std::move(operators), std::move(output)));
    return task;
}

bool IsTwoStageTopNPlan(const std::shared_ptr<plan::PlanNode>& root_plan,
                        const optimizer::PushdownPlan& pushdown) {
    if (root_plan == nullptr || root_plan->kind != plan::PlanNodeKind::Limit ||
        root_plan->inputs.size() != 1 || root_plan->inputs[0] == nullptr ||
        root_plan->inputs[0]->kind != plan::PlanNodeKind::Sort) {
        return false;
    }
    if (root_plan->limit().offset != 0 || root_plan->limit().n <= 0) {
        return false;
    }
    const auto& request = pushdown.request;
    return !request.order_by.empty() && request.limit.has_value() && !request.offset.has_value() &&
           request.group_by.empty() && !request.aggregate.has_value() &&
           !request.distinct.has_value();
}

absl::StatusOr<std::optional<ExecutionTask>> BuildTwoStageTopNExecutionTask(
    const std::shared_ptr<plan::PlanNode>& root_plan,
    const optimizer::PushdownPlan& pushdown,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (!IsTwoStageTopNPlan(root_plan, pushdown)) {
        return std::nullopt;
    }

    optimizer::PushdownPlan partial = pushdown;
    partial.request.partitioned_topn = true;
    auto runtime_or = create_connector_runtime(partial);
    if (!runtime_or.ok()) {
        return runtime_or.status();
    }
    auto splits_or = create_connector_splits(runtime_or->get(), partial);
    if (!splits_or.ok()) {
        return splits_or.status();
    }
    if (splits_or->size() <= 1) {
        return std::nullopt;
    }

    auto buffer = std::make_shared<ExchangeBuffer>();
    buffer->SetProducerCount(splits_or->size());
    std::vector<std::unique_ptr<Operator>> split_roots;
    split_roots.reserve(splits_or->size());
    for (auto& split : *splits_or) {
        split_roots.push_back(
            std::unique_ptr<Operator>(new ConnectorSplitScanOperator(std::move(split))));
    }
    auto producer_roots = WrapDriverRootsWithExchangeSinks(std::move(split_roots), buffer);

    ExecutionTask task;
    task.memory_context = memory_context;
    task.pipelines.push_back(MakePipeline("topn-partial",
                                          "topn partial",
                                          "source",
                                          {},
                                          {"ConnectorScanOperator", "ExchangeSinkOperator"},
                                          nullptr,
                                          std::move(producer_roots),
                                          GatherDistribution()));

    std::unique_ptr<Operator> current(new ExchangeSourceOperator(buffer));
    std::vector<std::string> operators = {current->name()};
    current = std::unique_ptr<Operator>(new TopNOperator(
        root_plan->inputs[0], static_cast<size_t>(root_plan->limit().n), std::move(current)));
    operators.push_back(current->name());
    std::unique_ptr<Operator> output(new OutputOperator(std::move(current)));
    operators.push_back(output->name());
    task.pipelines.push_back(MakePipeline(
        "main", "main", "root", {"topn-partial"}, std::move(operators), std::move(output)));
    return task;
}

std::shared_ptr<plan::PlanNode> SkipMaterializeBarrier(
    const std::shared_ptr<plan::PlanNode>& node) {
    if (node != nullptr && node->kind == plan::PlanNodeKind::Materialize &&
        node->inputs.size() == 1 && node->inputs[0] != nullptr) {
        return node->inputs[0];
    }
    return node;
}

absl::StatusOr<std::optional<ExecutionTask>> BuildTwoStageGroupedAggregateExecutionTask(
    const std::shared_ptr<plan::PlanNode>& root_plan,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (root_plan == nullptr) {
        return std::nullopt;
    }

    std::vector<std::shared_ptr<plan::PlanNode>> chain;
    std::shared_ptr<plan::PlanNode> cursor = root_plan;
    while (cursor != nullptr && cursor->inputs.size() == 1 && cursor->inputs[0] != nullptr) {
        chain.push_back(cursor);
        cursor = cursor->inputs[0];
    }

    std::optional<size_t> aggregate_index;
    for (size_t index = 0; index + 1 < chain.size(); ++index) {
        if (chain[index]->kind == plan::PlanNodeKind::Aggregate &&
            chain[index + 1]->kind == plan::PlanNodeKind::Group &&
            chain[index + 1]->inputs.size() == 1 && chain[index + 1]->inputs[0] != nullptr) {
            aggregate_index = index;
            break;
        }
    }
    if (!aggregate_index.has_value()) {
        return std::nullopt;
    }

    const auto& aggregate = chain[*aggregate_index];
    const auto& group = chain[*aggregate_index + 1];
    auto input_plan = SkipMaterializeBarrier(group->inputs[0]);
    ExecutionTask task;
    task.memory_context = memory_context;
    auto input_or = BuildParallelInputForPlan(input_plan, "aggregate-input", &task, memory_context);
    if (!input_or.ok()) {
        return input_or.status();
    }
    auto& planned = input_or->planned;
    if (planned.driver_roots.size() <= 1) {
        return std::nullopt;
    }

    const GroupedAggregateStrategy strategy = ChooseGroupedAggregateStrategy(
        root_plan, *aggregate_index, group, input_plan, planned.driver_roots.size());
    if (strategy == GroupedAggregateStrategy::SingleStage) {
        return std::nullopt;
    }

    if (strategy == GroupedAggregateStrategy::TwoStagePartitioned) {
        const size_t partition_count = planned.driver_roots.size();
        std::vector<std::shared_ptr<ExchangeBuffer>> buffers;
        buffers.reserve(partition_count);
        for (size_t index = 0; index < partition_count; ++index) {
            auto buffer = std::make_shared<ExchangeBuffer>();
            buffer->SetProducerCount(planned.driver_roots.size());
            buffers.push_back(std::move(buffer));
        }

        std::vector<std::unique_ptr<Operator>> partial_roots;
        partial_roots.reserve(planned.driver_roots.size());
        for (auto& root : planned.driver_roots) {
            std::unique_ptr<Operator> partial(
                new StreamingGroupedAggregateOperator(aggregate,
                                                      group->group().columns,
                                                      std::move(root),
                                                      memory_context,
                                                      AggregatePhase::Partial));
            auto distribution = HashDistribution({}, true, partition_count);
            partial_roots.push_back(std::unique_ptr<Operator>(
                new PartitionedExchangeSinkOperator(std::move(partial), buffers, distribution)));
        }

        std::vector<std::string> partial_operators = std::move(planned.operators);
        partial_operators.emplace_back("GroupOperator");
        partial_operators.emplace_back("PartialAggregateOperator");
        partial_operators.emplace_back("HashPartitionExchangeSinkOperator");

        task.pipelines.push_back(
            MakePipeline("aggregate-partial",
                         "aggregate partial partitioned",
                         "source",
                         input_or->dependencies,
                         std::move(partial_operators),
                         nullptr,
                         std::move(partial_roots),
                         HashDistribution(group->group().columns, true, partition_count)));

        std::vector<std::unique_ptr<Operator>> final_roots;
        final_roots.reserve(buffers.size());
        for (const auto& buffer : buffers) {
            std::unique_ptr<Operator> current(new ExchangeSourceOperator(buffer));
            current = std::unique_ptr<Operator>(
                new StreamingGroupedAggregateOperator(aggregate,
                                                      group->group().columns,
                                                      std::move(current),
                                                      memory_context,
                                                      AggregatePhase::Final));
            final_roots.push_back(std::move(current));
        }
        final_roots = WrapDriverRootsWithOutput(std::move(final_roots));
        std::vector<std::string> root_operators = {
            "ExchangeSourceOperator",
            "GroupOperator",
            "FinalAggregateOperator",
            "OutputOperator",
        };
        task.pipelines.push_back(
            MakePipeline("main",
                         "main partitioned final",
                         "root",
                         {"aggregate-partial"},
                         std::move(root_operators),
                         nullptr,
                         std::move(final_roots),
                         HashDistribution(group->group().columns, true, partition_count)));
        return task;
    }

    auto buffer = std::make_shared<ExchangeBuffer>();
    buffer->SetProducerCount(planned.driver_roots.size());
    std::vector<std::unique_ptr<Operator>> partial_roots;
    partial_roots.reserve(planned.driver_roots.size());
    for (auto& root : planned.driver_roots) {
        std::unique_ptr<Operator> partial(
            new StreamingGroupedAggregateOperator(aggregate,
                                                  group->group().columns,
                                                  std::move(root),
                                                  memory_context,
                                                  AggregatePhase::Partial));
        partial_roots.push_back(
            std::unique_ptr<Operator>(new ExchangeSinkOperator(std::move(partial), buffer)));
    }

    std::vector<std::string> partial_operators = std::move(planned.operators);
    partial_operators.emplace_back("GroupOperator");
    partial_operators.emplace_back("PartialAggregateOperator");
    partial_operators.emplace_back("ExchangeSinkOperator");

    task.pipelines.push_back(MakePipeline("aggregate-partial",
                                          "aggregate partial",
                                          "source",
                                          input_or->dependencies,
                                          std::move(partial_operators),
                                          nullptr,
                                          std::move(partial_roots),
                                          GatherDistribution()));

    std::unique_ptr<Operator> current(new ExchangeSourceOperator(buffer));
    std::vector<std::string> root_operators = {current->name(), "GroupOperator"};
    current =
        std::unique_ptr<Operator>(new StreamingGroupedAggregateOperator(aggregate,
                                                                        group->group().columns,
                                                                        std::move(current),
                                                                        memory_context,
                                                                        AggregatePhase::Final));
    root_operators.emplace_back("FinalAggregateOperator");
    for (size_t index = *aggregate_index; index > 0; --index) {
        auto wrapped_or = WrapUnaryOperator(chain[index - 1], std::move(current), memory_context);
        if (!wrapped_or.ok()) {
            return wrapped_or.status();
        }
        current = std::move(*wrapped_or);
        root_operators.push_back(current->name());
    }
    std::unique_ptr<Operator> output(new OutputOperator(std::move(current)));
    root_operators.push_back(output->name());
    task.pipelines.push_back(MakePipeline("main",
                                          "main",
                                          "root",
                                          {"aggregate-partial"},
                                          std::move(root_operators),
                                          std::move(output)));
    return task;
}

bool ShouldPartitionBlockingInput(const std::shared_ptr<plan::PlanNode>& input_plan,
                                  size_t driver_count) {
    constexpr double kSmallInputRows = 4096.0;
    if (driver_count <= 1) {
        return false;
    }
    const auto input_rows = EstimateRowsForPlan(input_plan);
    return !input_rows.has_value() || *input_rows >= kSmallInputRows;
}

absl::StatusOr<std::optional<ExecutionTask>> BuildPartitionedGroupExecutionTask(
    const std::shared_ptr<plan::PlanNode>& root_plan,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (root_plan == nullptr || root_plan->kind != plan::PlanNodeKind::Group ||
        root_plan->inputs.size() != 1 || root_plan->inputs[0] == nullptr ||
        root_plan->group().columns.empty()) {
        return std::nullopt;
    }
    auto input_plan = SkipMaterializeBarrier(root_plan->inputs[0]);
    ExecutionTask task;
    task.memory_context = memory_context;
    auto input_or = BuildParallelInputForPlan(input_plan, "group-input", &task, memory_context);
    if (!input_or.ok()) {
        return input_or.status();
    }
    auto& planned = input_or->planned;
    if (!ShouldPartitionBlockingInput(input_plan, planned.driver_roots.size())) {
        return std::nullopt;
    }

    const size_t partition_count = planned.driver_roots.size();
    std::vector<std::shared_ptr<ExchangeBuffer>> buffers;
    buffers.reserve(partition_count);
    for (size_t index = 0; index < partition_count; ++index) {
        auto buffer = std::make_shared<ExchangeBuffer>();
        buffer->SetProducerCount(planned.driver_roots.size());
        buffers.push_back(std::move(buffer));
    }

    std::vector<std::unique_ptr<Operator>> partial_roots;
    partial_roots.reserve(planned.driver_roots.size());
    for (auto& root : planned.driver_roots) {
        std::unique_ptr<Operator> partial(
            new StreamingGroupOperator(root_plan, std::move(root), memory_context, "partial"));
        partial_roots.push_back(std::unique_ptr<Operator>(new PartitionedExchangeSinkOperator(
            std::move(partial), buffers, HashDistribution({}, true, partition_count))));
    }

    std::vector<std::string> partial_operators = std::move(planned.operators);
    partial_operators.emplace_back("PartialGroupOperator");
    partial_operators.emplace_back("HashPartitionExchangeSinkOperator");

    task.pipelines.push_back(
        MakePipeline("group-partial",
                     "group partial partitioned",
                     "source",
                     input_or->dependencies,
                     std::move(partial_operators),
                     nullptr,
                     std::move(partial_roots),
                     HashDistribution(root_plan->group().columns, true, partition_count)));

    std::vector<std::unique_ptr<Operator>> final_roots;
    final_roots.reserve(buffers.size());
    for (const auto& buffer : buffers) {
        std::unique_ptr<Operator> current(new ExchangeSourceOperator(buffer));
        current = std::unique_ptr<Operator>(
            new StreamingGroupOperator(root_plan, std::move(current), memory_context, "final"));
        final_roots.push_back(std::move(current));
    }
    final_roots = WrapDriverRootsWithOutput(std::move(final_roots));
    task.pipelines.push_back(
        MakePipeline("main",
                     "main partitioned group",
                     "root",
                     {"group-partial"},
                     {"ExchangeSourceOperator", "FinalGroupOperator", "OutputOperator"},
                     nullptr,
                     std::move(final_roots),
                     HashDistribution(root_plan->group().columns, true, partition_count)));
    return task;
}

absl::StatusOr<std::optional<ExecutionTask>> BuildPartitionedDistinctExecutionTask(
    const std::shared_ptr<plan::PlanNode>& root_plan,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (root_plan == nullptr || root_plan->kind != plan::PlanNodeKind::Distinct ||
        root_plan->inputs.size() != 1 || root_plan->inputs[0] == nullptr ||
        root_plan->distinct().column.empty()) {
        return std::nullopt;
    }
    auto input_plan = SkipMaterializeBarrier(root_plan->inputs[0]);
    ExecutionTask task;
    task.memory_context = memory_context;
    auto input_or = BuildParallelInputForPlan(input_plan, "distinct-input", &task, memory_context);
    if (!input_or.ok()) {
        return input_or.status();
    }
    auto& planned = input_or->planned;
    if (!ShouldPartitionBlockingInput(input_plan, planned.driver_roots.size())) {
        return std::nullopt;
    }

    const size_t partition_count = planned.driver_roots.size();
    std::vector<std::shared_ptr<ExchangeBuffer>> buffers;
    buffers.reserve(partition_count);
    for (size_t index = 0; index < partition_count; ++index) {
        auto buffer = std::make_shared<ExchangeBuffer>();
        buffer->SetProducerCount(planned.driver_roots.size());
        buffers.push_back(std::move(buffer));
    }

    std::vector<std::unique_ptr<Operator>> partial_roots;
    partial_roots.reserve(planned.driver_roots.size());
    std::vector<std::string> partition_keys = {root_plan->distinct().column};
    for (auto& root : planned.driver_roots) {
        std::unique_ptr<Operator> partial(
            new StreamingDistinctOperator(root_plan, std::move(root), memory_context, "partial"));
        partial_roots.push_back(std::unique_ptr<Operator>(new PartitionedExchangeSinkOperator(
            std::move(partial), buffers, HashDistribution(partition_keys, true, partition_count))));
    }

    std::vector<std::string> partial_operators = std::move(planned.operators);
    partial_operators.emplace_back("PartialDistinctOperator");
    partial_operators.emplace_back("HashPartitionExchangeSinkOperator");

    task.pipelines.push_back(MakePipeline("distinct-partial",
                                          "distinct partial partitioned",
                                          "source",
                                          input_or->dependencies,
                                          std::move(partial_operators),
                                          nullptr,
                                          std::move(partial_roots),
                                          HashDistribution(partition_keys, true, partition_count)));

    std::vector<std::unique_ptr<Operator>> final_roots;
    final_roots.reserve(buffers.size());
    for (const auto& buffer : buffers) {
        std::unique_ptr<Operator> current(new ExchangeSourceOperator(buffer));
        current = std::unique_ptr<Operator>(
            new StreamingDistinctOperator(root_plan, std::move(current), memory_context, "final"));
        final_roots.push_back(std::move(current));
    }
    final_roots = WrapDriverRootsWithOutput(std::move(final_roots));
    task.pipelines.push_back(
        MakePipeline("main",
                     "main partitioned distinct",
                     "root",
                     {"distinct-partial"},
                     {"ExchangeSourceOperator", "FinalDistinctOperator", "OutputOperator"},
                     nullptr,
                     std::move(final_roots),
                     HashDistribution(partition_keys, true, partition_count)));
    return task;
}

absl::StatusOr<std::optional<ExecutionTask>> BuildUnaryBreakerExecutionTask(
    const std::shared_ptr<plan::PlanNode>& root_plan,
    const std::shared_ptr<QueryMemoryContext>& memory_context) {
    if (root_plan == nullptr) {
        return std::nullopt;
    }
    std::vector<std::shared_ptr<plan::PlanNode>> chain;
    std::shared_ptr<plan::PlanNode> cursor = root_plan;
    std::optional<size_t> breaker_index;
    while (cursor != nullptr && cursor->inputs.size() == 1 && cursor->inputs[0] != nullptr) {
        chain.push_back(cursor);
        if (IsPipelineBreakerKind(cursor->kind)) {
            breaker_index = chain.size() - 1;
        }
        cursor = cursor->inputs[0];
    }
    if (!breaker_index.has_value()) {
        return std::nullopt;
    }

    const auto& breaker = chain[*breaker_index];
    ExecutionTask task;
    task.memory_context = memory_context;
    auto input_or = AddProducerPipelineForPlan(
        breaker->inputs[0], "breaker-input", "source", &task, memory_context);
    if (!input_or.ok()) {
        return input_or.status();
    }

    std::unique_ptr<Operator> current(new ExchangeSourceOperator(input_or->buffer));
    std::vector<std::string> operators = {current->name()};
    size_t index = *breaker_index + 1;
    while (index > 0) {
        const auto& node = chain[index - 1];
        if (node->kind == plan::PlanNodeKind::Group && index >= 2 &&
            chain[index - 2]->kind == plan::PlanNodeKind::Aggregate) {
            const auto& aggregate = chain[index - 2];
            current = std::unique_ptr<Operator>(new StreamingGroupedAggregateOperator(
                aggregate, node->group().columns, std::move(current), memory_context));
            operators.emplace_back("GroupOperator");
            operators.push_back(current->name());
            index -= 2;
            continue;
        }
        auto wrapped_or = WrapUnaryOperator(node, std::move(current), memory_context);
        if (!wrapped_or.ok()) {
            return wrapped_or.status();
        }
        current = std::move(*wrapped_or);
        operators.push_back(current->name());
        --index;
    }
    std::unique_ptr<Operator> output(new OutputOperator(std::move(current)));
    operators.push_back(output->name());
    task.pipelines.push_back(MakePipeline(
        "main", "main", "root", {input_or->pipeline_id}, std::move(operators), std::move(output)));
    return task;
}

} // namespace

Value internal::ValueFromPage(const Page& page) {
    return value_from_page(page);
}

absl::StatusOr<ExecutionTask> PhysicalPlanner::Plan(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto memory_context = QueryMemoryContext::FromEnvironment();
    auto fast_or = optimizer::FastCostBasedOptimizer().OptimizeWithTrace(logical_plan);
    if (!fast_or.ok()) {
        return fast_or.status();
    }
    if (fast_or->rbo_result.plan != nullptr &&
        optimizer::ContainsPlanNodeKind(*fast_or->rbo_result.plan, plan::PlanNodeKind::Join)) {
        auto cbo_or = optimizer::DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);
        if (!cbo_or.ok()) {
            return cbo_or.status();
        }
        fast_or = std::move(cbo_or);
    }
    if (fast_or->rbo_result.plan != nullptr &&
        fast_or->rbo_result.plan->kind == plan::PlanNodeKind::Join) {
        return BuildUnaryWrappedJoinExecutionTask(fast_or->rbo_result.plan, memory_context);
    }
    if (fast_or->rbo_result.plan != nullptr &&
        fast_or->rbo_result.plan->kind == plan::PlanNodeKind::Exchange) {
        return BuildExchangeExecutionTask(fast_or->rbo_result.plan, memory_context);
    }
    if (fast_or->rbo_result.pushdown_plan.has_value() &&
        optimizer::CanExecutePushdownPlan(*fast_or->rbo_result.pushdown_plan)) {
        auto topn_task_or = BuildTwoStageTopNExecutionTask(
            fast_or->rbo_result.plan, *fast_or->rbo_result.pushdown_plan, memory_context);
        if (!topn_task_or.ok()) {
            return topn_task_or.status();
        }
        if (topn_task_or->has_value()) {
            return std::move(**topn_task_or);
        }
    }
    if (!fast_or->rbo_result.pushdown_plan.has_value() ||
        !optimizer::CanExecutePushdownPlan(*fast_or->rbo_result.pushdown_plan)) {
        auto aggregate_task_or =
            BuildTwoStageGroupedAggregateExecutionTask(fast_or->rbo_result.plan, memory_context);
        if (!aggregate_task_or.ok()) {
            return aggregate_task_or.status();
        }
        if (aggregate_task_or->has_value()) {
            return std::move(**aggregate_task_or);
        }
        auto group_task_or =
            BuildPartitionedGroupExecutionTask(fast_or->rbo_result.plan, memory_context);
        if (!group_task_or.ok()) {
            return group_task_or.status();
        }
        if (group_task_or->has_value()) {
            return std::move(**group_task_or);
        }
        auto distinct_task_or =
            BuildPartitionedDistinctExecutionTask(fast_or->rbo_result.plan, memory_context);
        if (!distinct_task_or.ok()) {
            return distinct_task_or.status();
        }
        if (distinct_task_or->has_value()) {
            return std::move(**distinct_task_or);
        }
        auto breaker_task_or =
            BuildUnaryBreakerExecutionTask(fast_or->rbo_result.plan, memory_context);
        if (!breaker_task_or.ok()) {
            return breaker_task_or.status();
        }
        if (breaker_task_or->has_value()) {
            return std::move(**breaker_task_or);
        }
    }
    if (FindUnaryWrappedJoinPlan(fast_or->rbo_result.plan).has_value()) {
        return BuildUnaryWrappedJoinExecutionTask(fast_or->rbo_result.plan, memory_context);
    }

    auto planned_or = BuildOperator(fast_or->rbo_result.plan, true, memory_context);
    if (!planned_or.ok()) {
        return planned_or.status();
    }
    if (!planned_or->driver_roots.empty()) {
        std::vector<std::string> operators = std::move(planned_or->operators);
        auto outputs = WrapDriverRootsWithOutput(std::move(planned_or->driver_roots));
        operators.emplace_back("OutputOperator");
        ExecutionTask task;
        task.memory_context = memory_context;
        task.pipelines.push_back(MakePipeline(
            "main", "main", "root", {}, std::move(operators), nullptr, std::move(outputs)));
        return task;
    }
    std::unique_ptr<Operator> output(new OutputOperator(std::move(planned_or->root)));
    planned_or->operators.push_back(output->name());

    ExecutionTask task;
    task.memory_context = memory_context;
    Pipeline pipeline;
    pipeline.id = "main";
    pipeline.name = "main";
    pipeline.role = "root";
    pipeline.operators = std::move(planned_or->operators);
    pipeline.root = std::move(output);
    task.pipelines.push_back(std::move(pipeline));
    return task;
}

} // namespace pl::flux::execution
