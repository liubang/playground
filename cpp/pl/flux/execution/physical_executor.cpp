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
#include <cstddef>
#include <optional>
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
            chunk.columns.push_back(ColumnVector{.name = source_column.name,
                                                 .type = source_column.type});
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
            chunk.columns.push_back(ColumnVector{.name = source_column.name,
                                                 .type = source_column.type});
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
            column.name = mapped_column_name(plan->rename().columns, column.name).value_or(
                column.name);
        }
    }
    return page_with_plan(std::move(input), plan);
}

Page materialize_page(Page input, const std::shared_ptr<plan::PlanNode>& plan) {
    input.plan = plan;
    input.materialized = true;
    return input;
}

Page exchange_page(Page input, const std::shared_ptr<plan::PlanNode>& plan) {
    input.plan = plan;
    return input;
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
    const std::unordered_set<std::string> left_column_set(left_columns.begin(),
                                                          left_columns.end());
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
    TableChunk chunk;
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

class ExchangeOperator final : public PageUnaryOperator {
public:
    using PageUnaryOperator::PageUnaryOperator;
    [[nodiscard]] std::string name() const override { return "ExchangeOperator"; }

private:
    absl::StatusOr<Page> Apply(Page input) const override {
        return exchange_page(std::move(input), plan());
    }
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
                const size_t take = std::min<size_t>(
                    source.row_count - start, static_cast<size_t>(remaining_limit_));
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
    std::vector<std::string> operators;
};

absl::StatusOr<PlannedOperator> BuildOperator(const std::shared_ptr<plan::PlanNode>& logical_plan) {
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
        auto left_or = BuildOperator(optimized_plan->inputs[0]);
        if (!left_or.ok()) {
            return left_or.status();
        }
        auto right_or = BuildOperator(optimized_plan->inputs[1]);
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
    auto planned_or = BuildOperator(optimized_plan->inputs[0]);
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

} // namespace

absl::StatusOr<ExecutionTask> PhysicalPlanner::Plan(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto planned_or = BuildOperator(logical_plan);
    if (!planned_or.ok()) {
        return planned_or.status();
    }
    std::unique_ptr<Operator> output(new OutputOperator(std::move(planned_or->root)));
    planned_or->operators.push_back(output->name());

    ExecutionTask task;
    Pipeline pipeline;
    pipeline.name = "main";
    pipeline.operators = std::move(planned_or->operators);
    pipeline.root = std::move(output);
    task.pipelines.push_back(std::move(pipeline));
    return task;
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
    Value value = value_from_page(*output);
    value.as_table_mut().materialized = true;
    return value;
}

absl::StatusOr<Value> Scheduler::Run(ExecutionTask task) const {
    if (task.pipelines.empty()) {
        return absl::InvalidArgumentError("execution task has no pipelines");
    }
    std::optional<TableValue> output;
    for (auto& pipeline : task.pipelines) {
        auto value_or = Driver(std::move(pipeline)).Run();
        if (!value_or.ok()) {
            return value_or.status();
        }
        const auto& table = value_or->as_table();
        if (!output.has_value()) {
            output = table;
            continue;
        }
        output->tables.insert(output->tables.end(), table.tables.begin(), table.tables.end());
        output->rows.insert(output->rows.end(), table.rows.begin(), table.rows.end());
    }
    if (!output.has_value()) {
        return Value::table_stream("", {});
    }
    Value value = Value::table_stream(output->bucket, output->tables, output->range_start,
                                      output->range_stop, output->result_name);
    value.as_table_mut().plan = output->plan;
    value.as_table_mut().materialized = true;
    return value;
}

absl::StatusOr<Value> PhysicalExecutor::Execute(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto operator_or = PhysicalPlanner().Plan(logical_plan);
    if (!operator_or.ok()) {
        return operator_or.status();
    }
    return Scheduler().Run(std::move(*operator_or));
}

} // namespace pl::flux::execution
