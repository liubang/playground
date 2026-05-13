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
#include "cpp/pl/flux/optimizer/cbo.h"
#include "cpp/pl/flux/runtime/runtime_builtin_aggregate_helpers.h"
#include "cpp/pl/flux/runtime/runtime_builtin_table_helpers.h"
#include <algorithm>
#include <cstddef>
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

absl::StatusOr<Value> execute_pushdown_plan(const optimizer::PushdownPlan& plan) {
    if (plan.source == nullptr) {
        return absl::InvalidArgumentError("pushdown plan has no source");
    }
    if (!optimizer::CanExecutePushdownPlan(plan)) {
        return absl::InvalidArgumentError("group without aggregate is not executable pushdown");
    }
    connector::SourceSpec spec{plan.source->source, plan.source->driver, plan.source->dsn,
                               plan.source->table};
    auto source_or = connector::ConnectorRegistry::Global().Create(spec);
    if (!source_or.ok()) {
        return source_or.status();
    }
    return (*source_or)->Scan(plan.request);
}

absl::StatusOr<Value> apply_range(const Value& input, const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& table = input.as_table();
    std::vector<TableChunk> chunks;
    chunks.reserve(table.table_count());
    for (const auto& chunk : table.tables) {
        TableChunk next;
        next.group_key = chunk.group_key;
        next.columns = chunk.columns;
        for (const auto& row : chunk.rows) {
            if (row != nullptr && time_in_range(*row, plan->range())) {
                next.rows.push_back(row);
            }
        }
        chunks.push_back(std::move(next));
    }
    return table_with_plan(table_with_chunks_like(table, std::move(chunks)), plan);
}

absl::StatusOr<Value> apply_filter(const Value& input,
                                   const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& table = input.as_table();
    std::vector<TableChunk> chunks;
    chunks.reserve(table.table_count());
    for (const auto& chunk : table.tables) {
        TableChunk next;
        next.group_key = chunk.group_key;
        next.columns = chunk.columns;
        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
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
            if (keep) {
                next.rows.push_back(row);
            }
        }
        chunks.push_back(std::move(next));
    }
    return table_with_plan(table_with_chunks_like(table, std::move(chunks)), plan);
}

absl::StatusOr<Value> apply_project(const Value& input,
                                    const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& table = input.as_table();
    const std::unordered_set<std::string> selected(plan->project().columns.begin(),
                                                   plan->project().columns.end());
    std::vector<TableChunk> chunks;
    chunks.reserve(table.table_count());
    for (const auto& chunk : table.tables) {
        TableChunk next;
        next.group_key = chunk.group_key;
        next.columns = plan->project().columns;
        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
            std::vector<std::pair<std::string, Value>> props;
            props.reserve(row->properties.size());
            for (const auto& [key, value] : row->properties) {
                if (selected.count(key) != 0) {
                    props.emplace_back(key, value);
                }
            }
            next.rows.push_back(std::make_shared<ObjectValue>(std::move(props)));
        }
        chunks.push_back(std::move(next));
    }
    return table_with_plan(table_with_chunks_like(table, std::move(chunks)), plan);
}

absl::StatusOr<Value> apply_rename(const Value& input,
                                   const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& table = input.as_table();
    std::vector<TableChunk> chunks;
    chunks.reserve(table.table_count());
    for (const auto& chunk : table.tables) {
        TableChunk next;
        next.group_key = chunk.group_key;
        next.columns.reserve(chunk.columns.size());
        for (const auto& column : chunk.columns) {
            next.columns.push_back(
                mapped_column_name(plan->rename().columns, column).value_or(column));
        }
        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
            std::vector<std::pair<std::string, Value>> props;
            props.reserve(row->properties.size());
            for (const auto& [key, value] : row->properties) {
                props.emplace_back(mapped_column_name(plan->rename().columns, key).value_or(key),
                                   value);
            }
            next.rows.push_back(std::make_shared<ObjectValue>(std::move(props)));
        }
        chunks.push_back(std::move(next));
    }
    return table_with_plan(table_with_chunks_like(table, std::move(chunks)), plan);
}

absl::StatusOr<Value> apply_limit(const Value& input, const std::shared_ptr<plan::PlanNode>& plan) {
    const auto& table = input.as_table();
    const size_t begin = static_cast<size_t>(std::max<int64_t>(0, plan->limit().offset));
    const size_t count = static_cast<size_t>(std::max<int64_t>(0, plan->limit().n));
    std::vector<TableChunk> chunks;
    chunks.reserve(table.table_count());
    for (const auto& chunk : table.tables) {
        TableChunk next;
        next.group_key = chunk.group_key;
        next.columns = chunk.columns;
        if (begin < chunk.rows.size()) {
            const size_t end = std::min(chunk.rows.size(), begin + count);
            next.rows.insert(next.rows.end(),
                             chunk.rows.begin() + static_cast<std::ptrdiff_t>(begin),
                             chunk.rows.begin() + static_cast<std::ptrdiff_t>(end));
        }
        chunks.push_back(std::move(next));
    }
    return table_with_plan(table_with_chunks_like(table, std::move(chunks)), plan);
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

class ConnectorScanOperator final : public Operator {
public:
    explicit ConnectorScanOperator(optimizer::PushdownPlan plan) : plan_(std::move(plan)) {}

    absl::StatusOr<Value> Next() override { return execute_pushdown_plan(plan_); }

private:
    optimizer::PushdownPlan plan_;
};

class MaterializeOperator final : public Operator {
public:
    MaterializeOperator(std::shared_ptr<plan::PlanNode> plan, std::unique_ptr<Operator> input)
        : plan_(std::move(plan)), input_(std::move(input)) {}

    absl::StatusOr<Value> Next() override {
        auto input_or = input_->Next();
        if (!input_or.ok()) {
            return input_or.status();
        }
        input_or->as_table_mut().plan = plan_;
        input_or->as_table_mut().materialized = true;
        return *input_or;
    }

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
};

class MemoryUnaryOperator final : public Operator {
public:
    MemoryUnaryOperator(std::shared_ptr<plan::PlanNode> plan, std::unique_ptr<Operator> input)
        : plan_(std::move(plan)), input_(std::move(input)) {}

    absl::StatusOr<Value> Next() override {
        auto input_or = input_->Next();
        if (!input_or.ok()) {
            return input_or.status();
        }
        switch (plan_->kind) {
            case plan::PlanNodeKind::Range:
                return apply_range(*input_or, plan_);
            case plan::PlanNodeKind::Filter:
                return apply_filter(*input_or, plan_);
            case plan::PlanNodeKind::Project:
                return apply_project(*input_or, plan_);
            case plan::PlanNodeKind::Rename:
                return apply_rename(*input_or, plan_);
            case plan::PlanNodeKind::Limit:
                return apply_limit(*input_or, plan_);
            case plan::PlanNodeKind::Sort:
                return apply_sort(*input_or, plan_);
            case plan::PlanNodeKind::Group:
                return apply_group(*input_or, plan_);
            case plan::PlanNodeKind::Distinct:
                return apply_distinct(*input_or, plan_);
            case plan::PlanNodeKind::Aggregate:
                return apply_aggregate(*input_or, plan_);
            default: {
                return absl::InvalidArgumentError(absl::StrCat(
                    "unsupported physical operator: ", plan::PlanNodeKindName(plan_->kind)));
            }
        }
    }

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
};

class OutputOperator final : public Operator {
public:
    explicit OutputOperator(std::unique_ptr<Operator> input) : input_(std::move(input)) {}

    absl::StatusOr<Value> Next() override {
        auto value_or = input_->Next();
        if (!value_or.ok()) {
            return value_or.status();
        }
        if (value_or->type() == Value::Type::Table) {
            value_or->as_table_mut().materialized = true;
        }
        return *value_or;
    }

private:
    std::unique_ptr<Operator> input_;
};

absl::StatusOr<std::unique_ptr<Operator>> BuildOperator(
    const std::shared_ptr<plan::PlanNode>& logical_plan) {
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
        return std::unique_ptr<Operator>(
            new ConnectorScanOperator(std::move(*optimized.pushdown_plan)));
    }
    const auto& optimized_plan = optimized.plan;
    if (optimized_plan->inputs.size() != 1 || optimized_plan->inputs[0] == nullptr) {
        return absl::InvalidArgumentError("plan is not executable");
    }
    auto input_or = BuildOperator(optimized_plan->inputs[0]);
    if (!input_or.ok()) {
        return input_or.status();
    }
    if (optimized_plan->kind == plan::PlanNodeKind::Materialize) {
        return std::unique_ptr<Operator>(
            new MaterializeOperator(optimized_plan, std::move(*input_or)));
    }
    return std::unique_ptr<Operator>(new MemoryUnaryOperator(optimized_plan, std::move(*input_or)));
}

} // namespace

absl::StatusOr<std::unique_ptr<Operator>> PhysicalPlanner::Plan(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto input_or = BuildOperator(logical_plan);
    if (!input_or.ok()) {
        return input_or.status();
    }
    return std::unique_ptr<Operator>(new OutputOperator(std::move(*input_or)));
}

Driver::Driver(std::unique_ptr<Operator> root) : root_(std::move(root)) {}

absl::StatusOr<Value> Driver::Run() {
    if (root_ == nullptr) {
        return absl::InvalidArgumentError("driver has no root operator");
    }
    return root_->Next();
}

absl::StatusOr<Value> PhysicalExecutor::Execute(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto operator_or = PhysicalPlanner().Plan(logical_plan);
    if (!operator_or.ok()) {
        return operator_or.status();
    }
    return Driver(std::move(*operator_or)).Run();
}

} // namespace pl::flux::execution
