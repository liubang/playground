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

#include <limits>
#include <optional>
#include <unordered_set>

#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/execution/materializer.h"
#include "cpp/pl/flux/optimizer/rbo.h"
#include "cpp/pl/flux/runtime/runtime_builtin_table_helpers.h"
#include "cpp/pl/flux/runtime/runtime_builtin_time_helpers.h"
#include "cpp/pl/flux/runtime/runtime_builtin_universe.h"
#include "cpp/pl/flux/syntax/ast.h"

namespace pl::flux {
namespace {
using namespace detail;

absl::StatusOr<Value> materialized_table_value(const TableValue& table) {
    Value value =
        table.materialized
            ? Value::table_stream(table.bucket,
                                  table.tables,
                                  table.range_start,
                                  table.range_stop,
                                  table.result_name)
            : Value::table_plan(
                  table.bucket, table.plan, table.range_start, table.range_stop, table.result_name);
    value.as_table_mut().plan = table.plan;
    return execution::MaterializeValue(std::move(value));
}

absl::StatusOr<const TableValue*> materialized_table_ref(const TableValue& table, Value* storage) {
    if (table.materialized) {
        return &table;
    }
    auto value_or = materialized_table_value(table);
    if (!value_or.ok()) {
        return value_or.status();
    }
    *storage = std::move(*value_or);
    return &storage->as_table();
}

absl::StatusOr<std::string> predicate_property_name(const PropertyKey& key) {
    switch (key.type) {
        case PropertyKey::Type::Identifier:
            return std::get<std::unique_ptr<Identifier>>(key.key)->name;
        case PropertyKey::Type::StringLiteral:
            return std::get<std::unique_ptr<StringLit>>(key.key)->value;
    }
    return absl::InvalidArgumentError("unsupported predicate property key");
}

absl::StatusOr<std::string> predicate_parameter_name(const Property& param) {
    return predicate_property_name(*param.key);
}

std::optional<std::string> row_member_column(const Expression& expr,
                                             const std::string& row_param_name) {
    if (expr.type != Expression::Type::MemberExpr) {
        return std::nullopt;
    }
    const auto& member = std::get<std::unique_ptr<MemberExpr>>(expr.expr);
    if (member->object->type != Expression::Type::Identifier) {
        return std::nullopt;
    }
    const auto& root = std::get<std::unique_ptr<Identifier>>(member->object->expr);
    if (root->name != row_param_name) {
        return std::nullopt;
    }
    auto column_or = predicate_property_name(*member->property);
    if (!column_or.ok()) {
        return std::nullopt;
    }
    return *column_or;
}

std::optional<plan::PredicateLiteral> predicate_literal(const Expression& expr) {
    switch (expr.type) {
        case Expression::Type::BooleanLit:
            return plan::PredicateLiteral{
                .kind = plan::PredicateLiteralKind::Bool,
                .bool_value = std::get<std::unique_ptr<BooleanLit>>(expr.expr)->value,
            };
        case Expression::Type::IntegerLit:
            return plan::PredicateLiteral{
                .kind = plan::PredicateLiteralKind::Int,
                .int_value = std::get<std::unique_ptr<IntegerLit>>(expr.expr)->value,
            };
        case Expression::Type::UnsignedIntegerLit:
            return plan::PredicateLiteral{
                .kind = plan::PredicateLiteralKind::UInt,
                .uint_value = std::get<std::unique_ptr<UintLit>>(expr.expr)->value,
            };
        case Expression::Type::FloatLit:
            return plan::PredicateLiteral{
                .kind = plan::PredicateLiteralKind::Float,
                .float_value = std::get<std::unique_ptr<FloatLit>>(expr.expr)->value,
            };
        case Expression::Type::StringLit:
            return plan::PredicateLiteral{
                .kind = plan::PredicateLiteralKind::String,
                .string_value = std::get<std::unique_ptr<StringLit>>(expr.expr)->value,
            };
        case Expression::Type::DateTimeLit:
            return plan::PredicateLiteral{
                .kind = plan::PredicateLiteralKind::Time,
                .string_value = std::get<std::unique_ptr<DateTimeLit>>(expr.expr)->string(),
            };
        default:
            return std::nullopt;
    }
}

std::optional<plan::PredicateOp> predicate_op(Operator op) {
    switch (op) {
        case Operator::EqualOperator:
            return plan::PredicateOp::Eq;
        case Operator::NotEqualOperator:
            return plan::PredicateOp::NotEq;
        case Operator::LessThanOperator:
            return plan::PredicateOp::Lt;
        case Operator::LessThanEqualOperator:
            return plan::PredicateOp::Lte;
        case Operator::GreaterThanOperator:
            return plan::PredicateOp::Gt;
        case Operator::GreaterThanEqualOperator:
            return plan::PredicateOp::Gte;
        default:
            return std::nullopt;
    }
}

plan::PredicateOp reverse_predicate_op(plan::PredicateOp op) {
    switch (op) {
        case plan::PredicateOp::Eq:
            return plan::PredicateOp::Eq;
        case plan::PredicateOp::NotEq:
            return plan::PredicateOp::NotEq;
        case plan::PredicateOp::Lt:
            return plan::PredicateOp::Gt;
        case plan::PredicateOp::Lte:
            return plan::PredicateOp::Gte;
        case plan::PredicateOp::Gt:
            return plan::PredicateOp::Lt;
        case plan::PredicateOp::Gte:
            return plan::PredicateOp::Lte;
    }
    return plan::PredicateOp::Eq;
}

std::optional<plan::PredicateSpec> extract_binary_predicate(const BinaryExpr& binary,
                                                            const std::string& row_param_name) {
    auto op = predicate_op(binary.op);
    if (!op.has_value()) {
        return std::nullopt;
    }

    if (auto column = row_member_column(*binary.left, row_param_name); column.has_value()) {
        auto literal = predicate_literal(*binary.right);
        if (!literal.has_value()) {
            return std::nullopt;
        }
        return plan::PredicateSpec{
            .op = *op,
            .column = *column,
            .literal = *literal,
        };
    }
    if (auto column = row_member_column(*binary.right, row_param_name); column.has_value()) {
        auto literal = predicate_literal(*binary.left);
        if (!literal.has_value()) {
            return std::nullopt;
        }
        return plan::PredicateSpec{
            .op = reverse_predicate_op(*op),
            .column = *column,
            .literal = *literal,
        };
    }
    return std::nullopt;
}

bool extract_predicates_from_expr(const Expression& expr,
                                  const std::string& row_param_name,
                                  std::vector<plan::PredicateSpec>* predicates) {
    if (expr.type == Expression::Type::LogicalExpr) {
        const auto& logical = std::get<std::unique_ptr<LogicalExpr>>(expr.expr);
        if (logical->op != LogicalOperator::AndOperator) {
            return false;
        }
        return extract_predicates_from_expr(*logical->left, row_param_name, predicates) &&
               extract_predicates_from_expr(*logical->right, row_param_name, predicates);
    }
    if (expr.type != Expression::Type::BinaryExpr) {
        return false;
    }
    const auto& binary = std::get<std::unique_ptr<BinaryExpr>>(expr.expr);
    auto predicate = extract_binary_predicate(*binary, row_param_name);
    if (!predicate.has_value()) {
        return false;
    }
    predicates->push_back(std::move(*predicate));
    return true;
}

std::optional<std::vector<plan::PredicateSpec>> extract_filter_predicates(const FunctionValue& fn) {
    if (fn.kind != FunctionValue::Kind::User || fn.user_function == nullptr ||
        fn.user_function->params.size() != 1 || fn.user_function->body == nullptr ||
        fn.user_function->body->type != FunctionBody::Type::Expression) {
        return std::nullopt;
    }
    auto row_param_or = predicate_parameter_name(*fn.user_function->params[0]);
    if (!row_param_or.ok()) {
        return std::nullopt;
    }
    const auto& body = *std::get<std::unique_ptr<Expression>>(fn.user_function->body->body);
    std::vector<plan::PredicateSpec> predicates;
    if (!extract_predicates_from_expr(body, *row_param_or, &predicates) || predicates.empty()) {
        return std::nullopt;
    }
    return predicates;
}

} // namespace

namespace detail {

Value with_aggregate_plan(Value value,
                          const TableValue& input,
                          plan::AggregateFunction fn,
                          std::string column) {
    if (input.plan != nullptr) {
        auto node = plan::MakeAggregate(input.plan, fn, std::move(column));
        if (!input.materialized) {
            return Value::table_plan(input.bucket,
                                     std::move(node),
                                     input.range_start,
                                     input.range_stop,
                                     input.result_name);
        }
        value.as_table_mut().plan = std::move(node);
    }
    return value;
}

Value with_distinct_plan(Value value, const TableValue& input, std::string column) {
    if (input.plan != nullptr) {
        auto node = plan::MakeDistinct(input.plan, std::move(column));
        if (!input.materialized) {
            return Value::table_plan(input.bucket,
                                     std::move(node),
                                     input.range_start,
                                     input.range_stop,
                                     input.result_name);
        }
        value.as_table_mut().plan = std::move(node);
    }
    return value;
}

} // namespace detail

namespace {

Value with_filter_plan(Value value,
                       const TableValue& input,
                       std::optional<std::vector<plan::PredicateSpec>> predicates) {
    if (input.plan != nullptr) {
        if (predicates.has_value()) {
            value.as_table_mut().plan = plan::MakeFilter(input.plan, std::move(*predicates));
            return value;
        }
        value.as_table_mut().plan =
            plan::MakeMaterializeBarrier(input.plan, "unsupported lazy builtin", "filter");
    }
    return value;
}

Value with_range_plan(Value value,
                      const TableValue& input,
                      std::string start,
                      std::optional<std::string> stop) {
    if (input.plan != nullptr) {
        value.as_table_mut().plan = plan::MakeRange(input.plan, std::move(start), std::move(stop));
    }
    return value;
}

Value with_project_plan(Value value, const TableValue& input, std::vector<std::string> columns) {
    if (input.plan != nullptr) {
        value.as_table_mut().plan = plan::MakeProject(input.plan, std::move(columns));
    }
    return value;
}

Value with_drop_plan(Value value,
                     const TableValue& input,
                     const std::vector<std::string>& dropped) {
    if (input.plan == nullptr) {
        return value;
    }
    auto columns_or = optimizer::VisibleColumnsForPlan(input.plan);
    if (!columns_or.ok()) {
        value.as_table_mut().plan =
            plan::MakeMaterializeBarrier(input.plan, "unsupported lazy builtin", "drop");
        return value;
    }
    const std::unordered_set<std::string> dropped_set(dropped.begin(), dropped.end());
    std::vector<std::string> projected_columns;
    projected_columns.reserve(columns_or->size());
    for (const auto& column : *columns_or) {
        if (dropped_set.count(column) == 0) {
            projected_columns.push_back(column);
        }
    }
    value.as_table_mut().plan = plan::MakeProject(input.plan, std::move(projected_columns));
    return value;
}

Value with_rename_plan(Value value,
                       const TableValue& input,
                       std::vector<std::pair<std::string, std::string>> columns) {
    if (input.plan == nullptr) {
        return value;
    }
    auto node = plan::MakeRename(input.plan, std::move(columns));
    auto visible_columns_or = optimizer::VisibleColumnsForPlan(node);
    if (!visible_columns_or.ok()) {
        value.as_table_mut().plan =
            plan::MakeMaterializeBarrier(input.plan, "unsupported lazy builtin", "rename");
        return value;
    }
    value.as_table_mut().plan = std::move(node);
    return value;
}

Value with_limit_plan(Value value, const TableValue& input, int64_t n, int64_t offset) {
    if (input.plan != nullptr) {
        value.as_table_mut().plan = plan::MakeLimit(input.plan, n, offset);
    }
    return value;
}

Value with_sort_plan(Value value,
                     const TableValue& input,
                     const std::vector<std::string>& columns,
                     bool desc) {
    if (input.plan != nullptr) {
        std::vector<plan::SortKey> keys;
        keys.reserve(columns.size());
        for (const auto& column : columns) {
            keys.push_back({
                .column = column,
                .desc = desc,
            });
        }
        value.as_table_mut().plan = plan::MakeSort(input.plan, std::move(keys));
    }
    return value;
}

Value with_group_plan(Value value, const TableValue& input, std::vector<std::string> columns) {
    if (input.plan != nullptr) {
        value.as_table_mut().plan = plan::MakeGroup(input.plan, std::move(columns));
    }
    return value;
}

Value with_materialization_barrier(Value value,
                                   const TableValue& input,
                                   const std::string& builtin) {
    if (input.plan != nullptr) {
        value.as_table_mut().plan =
            plan::MakeMaterializeBarrier(input.plan, "unsupported lazy builtin", builtin);
    }
    return value;
}

Value lazy_table_with_plan(const TableValue& input, std::shared_ptr<plan::PlanNode> node) {
    return Value::table_plan(
        input.bucket, std::move(node), input.range_start, input.range_stop, input.result_name);
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
    if (!(*table_or)->materialized && (*table_or)->plan != nullptr) {
        auto result =
            lazy_table_with_plan(**table_or, plan::MakeRange((*table_or)->plan, start, stop));
        result.as_table_mut().range_start = start;
        result.as_table_mut().range_stop = stop;
        return result;
    }

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
    auto result = Value::table_stream(table.bucket,
                                      std::move(table.tables),
                                      table.range_start,
                                      table.range_stop,
                                      table.result_name);
    return with_range_plan(std::move(result), **table_or, start, stop);
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
    std::optional<std::vector<plan::PredicateSpec>> predicates;
    if (empty_policy == EmptyChunkPolicy::Drop) {
        predicates = extract_filter_predicates((*fn_or)->as_function());
    }
    Value materialized_input;
    if (!(*table_or)->materialized && (*table_or)->plan != nullptr) {
        if (predicates.has_value()) {
            return lazy_table_with_plan(
                **table_or,
                plan::MakeFilter((*table_or)->plan, std::vector<plan::PredicateSpec>(*predicates)));
        }
        auto materialized_or = materialized_table_value(**table_or);
        if (!materialized_or.ok()) {
            return materialized_or.status();
        }
        materialized_input = *materialized_or;
        table_or = &materialized_input.as_table();
    }

    auto result_or = transform_rows_preserving_chunks(
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
    if (!result_or.ok()) {
        return result_or.status();
    }
    return with_filter_plan(std::move(*result_or), **table_or, std::move(predicates));
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
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;

    auto result_or = transform_rows_preserving_chunks(
        *table, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            auto mapped_or = ExpressionEvaluator::Invoke(**fn_or, {Value::object(clone_row(row))});
            if (!mapped_or.ok()) {
                return mapped_or.status();
            }
            if (mapped_or->type() != Value::Type::Object) {
                return absl::InvalidArgumentError("map `fn` must return an object");
            }
            return std::make_shared<ObjectValue>(mapped_or->as_object());
        });
    if (!result_or.ok()) {
        return result_or.status();
    }
    return with_materialization_barrier(std::move(*result_or), **table_or, "map");
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
            if (offset_value->as_uint() >
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return absl::InvalidArgumentError("limit `offset` overflows int64");
            }
            offset = static_cast<int64_t>(offset_value->as_uint());
        } else {
            return absl::InvalidArgumentError("limit `offset` must be an int or uint");
        }
        if (offset < 0) {
            return absl::InvalidArgumentError("limit `offset` must be non-negative");
        }
    }

    if (!(*table_or)->materialized && (*table_or)->plan != nullptr) {
        return lazy_table_with_plan(**table_or, plan::MakeLimit((*table_or)->plan, *n_or, offset));
    }
    const size_t begin = static_cast<size_t>(offset);
    auto result = slice_table_like(**table_or, [&](size_t size) {
        const size_t end = std::min(size, begin + static_cast<size_t>(*n_or));
        return std::pair<size_t, size_t>{begin, end};
    });
    return with_limit_plan(std::move(result), **table_or, *n_or, offset);
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
            if (offset_value->as_uint() >
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return absl::InvalidArgumentError("tail `offset` overflows int64");
            }
            offset = static_cast<int64_t>(offset_value->as_uint());
        } else {
            return absl::InvalidArgumentError("tail `offset` must be an int or uint");
        }
        if (offset < 0) {
            return absl::InvalidArgumentError("tail `offset` must be non-negative");
        }
    }

    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;
    auto result = slice_table_like(*table, [&](size_t row_count) {
        const size_t tail_end =
            offset >= static_cast<int64_t>(row_count) ? 0 : row_count - static_cast<size_t>(offset);
        const size_t tail_begin =
            static_cast<size_t>(*n_or) >= tail_end ? 0 : tail_end - static_cast<size_t>(*n_or);
        return std::pair<size_t, size_t>{tail_begin, tail_end};
    });
    return with_materialization_barrier(std::move(result), **table_or, "tail");
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
    if (!(*table_or)->materialized && (*table_or)->plan != nullptr) {
        return lazy_table_with_plan(**table_or, plan::MakeProject((*table_or)->plan, *columns_or));
    }
    const std::unordered_set<std::string> selected(columns_or->begin(), columns_or->end());
    auto result_or = transform_rows_preserving_chunks(
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
    if (!result_or.ok()) {
        return result_or.status();
    }
    return with_project_plan(std::move(*result_or), **table_or, *columns_or);
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
    if (!(*table_or)->materialized && (*table_or)->plan != nullptr) {
        auto visible_or = optimizer::VisibleColumnsForPlan((*table_or)->plan);
        if (!visible_or.ok()) {
            return visible_or.status();
        }
        const std::unordered_set<std::string> dropped(columns_or->begin(), columns_or->end());
        std::vector<std::string> projected;
        projected.reserve(visible_or->size());
        for (const auto& column : *visible_or) {
            if (dropped.count(column) == 0) {
                projected.push_back(column);
            }
        }
        return lazy_table_with_plan(**table_or,
                                    plan::MakeProject((*table_or)->plan, std::move(projected)));
    }
    const std::unordered_set<std::string> dropped(columns_or->begin(), columns_or->end());
    auto result_or = transform_rows_preserving_chunks(
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
    if (!result_or.ok()) {
        return result_or.status();
    }
    return with_drop_plan(std::move(*result_or), **table_or, *columns_or);
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
    if (!(*table_or)->materialized && (*table_or)->plan != nullptr) {
        auto result =
            lazy_table_with_plan(**table_or, plan::MakeRename((*table_or)->plan, *columns_or));
        auto visible_or = optimizer::VisibleColumnsForPlan(result.as_table().plan);
        if (!visible_or.ok()) {
            return visible_or.status();
        }
        (void)visible_or;
        return result;
    }

    auto result_or = transform_rows_preserving_chunks(
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
    if (!result_or.ok()) {
        return result_or.status();
    }
    return with_rename_plan(std::move(*result_or), **table_or, *columns_or);
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
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;

    auto result_or = transform_rows_preserving_chunks(
        *table, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            const Value* value = row.lookup(*column_or);
            if (value == nullptr) {
                return clone_row(row);
            }
            auto duplicated = object_with_upserted_property(row, *as_or, *value);
            return std::make_shared<ObjectValue>(duplicated.as_object());
        });
    if (!result_or.ok()) {
        return result_or.status();
    }
    return with_materialization_barrier(std::move(*result_or), **table_or, "duplicate");
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
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;

    auto result_or = transform_rows_preserving_chunks(
        *table, [&](const ObjectValue& row) -> absl::StatusOr<std::shared_ptr<ObjectValue>> {
            auto updated = object_with_upserted_property(row, *key_or, **value_or);
            return std::make_shared<ObjectValue>(updated.as_object());
        });
    if (!result_or.ok()) {
        return result_or.status();
    }
    return with_materialization_barrier(std::move(*result_or), **table_or, "set");
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
    if (!(*table_or)->materialized && (*table_or)->plan != nullptr) {
        std::vector<plan::SortKey> keys;
        keys.reserve(columns_or->size());
        for (const auto& column : *columns_or) {
            keys.push_back({
                .column = column,
                .desc = *desc_or,
            });
        }
        return lazy_table_with_plan(**table_or, plan::MakeSort((*table_or)->plan, std::move(keys)));
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
    auto result = table_with_chunks_like(**table_or, std::move(chunks));
    return with_sort_plan(std::move(result), **table_or, *columns_or, *desc_or);
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
    if (!(*table_or)->materialized && (*table_or)->plan != nullptr) {
        std::vector<std::string> group_columns = *columns_or;
        if (*mode_or == "except") {
            auto visible_or = optimizer::VisibleColumnsForPlan((*table_or)->plan);
            if (!visible_or.ok()) {
                return visible_or.status();
            }
            group_columns.clear();
            const std::unordered_set<std::string> excluded(columns_or->begin(), columns_or->end());
            for (const auto& column : *visible_or) {
                if (excluded.count(column) == 0) {
                    group_columns.push_back(column);
                }
            }
        }
        return lazy_table_with_plan(**table_or,
                                    plan::MakeGroup((*table_or)->plan, std::move(group_columns)));
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;

    std::vector<std::string> group_columns = *columns_or;
    if (*mode_or == "except") {
        group_columns.clear();
        const std::unordered_set<std::string> excluded(columns_or->begin(), columns_or->end());
        for (const auto& column : all_visible_columns_in_order(*table)) {
            if (excluded.count(column) == 0) {
                group_columns.push_back(column);
            }
        }
    }

    std::vector<TableChunk> chunks;
    std::unordered_map<std::string, size_t> chunk_indexes;
    chunks.reserve(table->rows.size());
    for (const auto& row : table->rows) {
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
    auto result = table_with_chunks_like(*table, std::move(chunks));
    return with_group_plan(std::move(result), **table_or, std::move(group_columns));
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
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;
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
    chunks.reserve(table->table_count());
    for (const auto& chunk : table->tables) {
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
    auto result = table_with_chunks_like(*table, std::move(chunks));
    return with_materialization_barrier(std::move(result), **table_or, "pivot");
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
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve(table->rows.size());
    std::unordered_map<std::string, Value> previous_by_group;
    for (const auto& row : table->rows) {
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
    auto result =
        Value::table(table->bucket, std::move(rows), table->range_start, table->range_stop);
    return with_materialization_barrier(std::move(result), **table_or, "fill");
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
    std::vector<Value> materialized_tables;
    materialized_tables.reserve(tables_or->size());
    for (const auto* table : *tables_or) {
        if (table == nullptr) {
            continue;
        }
        if (!table->materialized) {
            auto materialized_or = materialized_table_value(*table);
            if (!materialized_or.ok()) {
                return materialized_or.status();
            }
            materialized_tables.push_back(std::move(*materialized_or));
            table = &materialized_tables.back().as_table();
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
    auto result = Value::table_stream(bucket, std::move(chunks), range_start, range_stop);
    return detail::with_materialization_barrier(std::move(result), *tables_or, "union");
}

} // namespace

void InstallUniverseTransformBuiltins(Environment& env) {
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

} // namespace pl::flux
