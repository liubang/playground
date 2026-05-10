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

#include "cpp/pl/flux/optimizer/source_pushdown.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/mysql_source.h"
#include "cpp/pl/flux/connector/sqlite_source.h"
#include "cpp/pl/flux/optimizer/rbo.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace pl::flux::optimizer {
namespace {

connector::PredicateOp to_connector_predicate_op(plan::PredicateOp op) {
    switch (op) {
        case plan::PredicateOp::Eq:
            return connector::PredicateOp::Eq;
        case plan::PredicateOp::NotEq:
            return connector::PredicateOp::NotEq;
        case plan::PredicateOp::Lt:
            return connector::PredicateOp::Lt;
        case plan::PredicateOp::Lte:
            return connector::PredicateOp::Lte;
        case plan::PredicateOp::Gt:
            return connector::PredicateOp::Gt;
        case plan::PredicateOp::Gte:
            return connector::PredicateOp::Gte;
    }
    return connector::PredicateOp::Eq;
}

connector::AggregateFunction to_connector_aggregate_fn(plan::AggregateFunction fn) {
    switch (fn) {
        case plan::AggregateFunction::Count:
            return connector::AggregateFunction::Count;
        case plan::AggregateFunction::Sum:
            return connector::AggregateFunction::Sum;
        case plan::AggregateFunction::Mean:
            return connector::AggregateFunction::Mean;
        case plan::AggregateFunction::Min:
            return connector::AggregateFunction::Min;
        case plan::AggregateFunction::Max:
            return connector::AggregateFunction::Max;
    }
    return connector::AggregateFunction::Count;
}

std::string connector_predicate_op_string(connector::PredicateOp op) {
    switch (op) {
        case connector::PredicateOp::Eq:
            return "==";
        case connector::PredicateOp::NotEq:
            return "!=";
        case connector::PredicateOp::Lt:
            return "<";
        case connector::PredicateOp::Lte:
            return "<=";
        case connector::PredicateOp::Gt:
            return ">";
        case connector::PredicateOp::Gte:
            return ">=";
    }
    return "==";
}

std::string connector_aggregate_fn_string(connector::AggregateFunction fn) {
    switch (fn) {
        case connector::AggregateFunction::Count:
            return "COUNT";
        case connector::AggregateFunction::Sum:
            return "SUM";
        case connector::AggregateFunction::Mean:
            return "AVG";
        case connector::AggregateFunction::Min:
            return "MIN";
        case connector::AggregateFunction::Max:
            return "MAX";
    }
    return "COUNT";
}

Value to_connector_literal(const plan::PredicateLiteral& literal) {
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

void append_string_list(std::ostringstream* out, const std::vector<std::string>& values) {
    *out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            *out << ", ";
        }
        *out << values[i];
    }
    *out << "]";
}

std::string projection_summary(const connector::ScanRequest& request) {
    std::ostringstream out;
    out << "[";
    if (!request.projection_columns.empty()) {
        for (size_t i = 0; i < request.projection_columns.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            const auto& projection = request.projection_columns[i];
            out << projection.column;
            if (!projection.alias.empty() && projection.alias != projection.column) {
                out << " AS " << projection.alias;
            }
        }
    } else if (!request.columns.empty()) {
        for (size_t i = 0; i < request.columns.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << request.columns[i];
        }
    } else {
        out << "*";
    }
    out << "]";
    return out.str();
}

void append_pushdown_summary_field(std::ostringstream* out,
                                   bool* needs_separator,
                                   const std::string& name,
                                   const std::string& value) {
    if (*needs_separator) {
        *out << ", ";
    }
    *out << name << "=" << value;
    *needs_separator = true;
}

absl::StatusOr<size_t> visible_column_index(const std::vector<std::string>& visible_columns,
                                            const std::string& column,
                                            const std::string& context) {
    auto it = std::find(visible_columns.begin(), visible_columns.end(), column);
    if (it == visible_columns.end()) {
        return absl::InvalidArgumentError(
            absl::StrCat("pushdown ", context, " references unavailable column: ", column));
    }
    return static_cast<size_t>(it - visible_columns.begin());
}

absl::StatusOr<std::string> source_column_for_visible(
    const std::vector<std::string>& visible_columns,
    const std::vector<std::string>& source_columns,
    const std::string& column,
    const std::string& context) {
    auto index_or = visible_column_index(visible_columns, column, context);
    if (!index_or.ok()) {
        return index_or.status();
    }
    if (*index_or >= source_columns.size()) {
        return absl::InvalidArgumentError("pushdown column mapping is inconsistent");
    }
    return source_columns[*index_or];
}

std::optional<std::string> mapped_column_name(
    const std::vector<std::pair<std::string, std::string>>& mappings, const std::string& column) {
    for (const auto& [from, to] : mappings) {
        if (from == column) {
            return to;
        }
    }
    return std::nullopt;
}

bool has_duplicate_columns(const std::vector<std::string>& columns) {
    std::unordered_set<std::string> seen;
    seen.reserve(columns.size());
    for (const auto& column : columns) {
        if (!seen.insert(column).second) {
            return true;
        }
    }
    return false;
}

void set_projection_columns(connector::ScanRequest* request,
                            const std::vector<std::string>& source_columns,
                            const std::vector<std::string>& visible_columns) {
    request->columns.clear();
    request->projection_columns.clear();
    request->projection_columns.reserve(visible_columns.size());
    for (size_t i = 0; i < visible_columns.size(); ++i) {
        request->projection_columns.push_back({
            .column = source_columns[i],
            .alias = visible_columns[i],
        });
    }
}

bool is_sql_provider_source(const plan::SourceScanSpec& source) {
    return (source.source == "sqlite" && source.driver == "sqlite") ||
           (source.source == "mysql" && source.driver == "mysql");
}

} // namespace

absl::StatusOr<std::vector<std::string>> SourceScanColumns(const plan::SourceScanSpec& source) {
    absl::StatusOr<connector::TableSchema> schema_or =
        absl::InvalidArgumentError("unsupported pushdown source");
    if (source.source == "sqlite" && source.driver == "sqlite") {
        connector::SQLiteSource sqlite_source(source.dsn, source.table);
        schema_or = sqlite_source.Schema();
    } else if (source.source == "mysql" && source.driver == "mysql") {
        connector::MySQLSource mysql_source(source.dsn, source.table);
        schema_or = mysql_source.Schema();
    }
    if (!schema_or.ok()) {
        return schema_or.status();
    }
    std::vector<std::string> columns;
    columns.reserve(schema_or->columns.size());
    for (const auto& column : schema_or->columns) {
        columns.push_back(column.name);
    }
    return columns;
}

absl::StatusOr<std::vector<std::string>> VisibleColumnsForPlan(
    const std::shared_ptr<plan::PlanNode>& node) {
    if (node == nullptr) {
        return absl::InvalidArgumentError("missing plan");
    }
    if (node->kind == plan::PlanNodeKind::SourceScan) {
        return SourceScanColumns(node->source_scan);
    }
    if (node->inputs.size() != 1) {
        return absl::InvalidArgumentError("non-linear plan has no stable visible columns");
    }
    auto columns_or = VisibleColumnsForPlan(node->inputs[0]);
    if (!columns_or.ok()) {
        return columns_or.status();
    }
    if (node->kind == plan::PlanNodeKind::Project) {
        return node->project.columns;
    }
    if (node->kind == plan::PlanNodeKind::Rename) {
        for (auto& column : *columns_or) {
            if (auto renamed = mapped_column_name(node->rename.columns, column);
                renamed.has_value()) {
                column = *renamed;
            }
        }
        if (has_duplicate_columns(*columns_or)) {
            return absl::InvalidArgumentError("rename produces duplicate columns");
        }
        return columns_or;
    }
    if (node->kind == plan::PlanNodeKind::Range || node->kind == plan::PlanNodeKind::Filter ||
        node->kind == plan::PlanNodeKind::Limit || node->kind == plan::PlanNodeKind::Sort ||
        node->kind == plan::PlanNodeKind::Group || node->kind == plan::PlanNodeKind::Aggregate ||
        node->kind == plan::PlanNodeKind::Distinct) {
        return columns_or;
    }
    return absl::InvalidArgumentError("plan node has no stable visible columns");
}

namespace {

absl::StatusOr<PushdownPlan> build_pushdown_plan(const std::shared_ptr<plan::PlanNode>& node) {
    if (node == nullptr) {
        return absl::InvalidArgumentError("missing plan");
    }
    if (node->kind == plan::PlanNodeKind::SourceScan) {
        if (!is_sql_provider_source(node->source_scan)) {
            return absl::InvalidArgumentError("unsupported pushdown source");
        }
        PushdownPlan plan;
        plan.source = &node->source_scan;
        auto columns_or = SourceScanColumns(node->source_scan);
        if (!columns_or.ok()) {
            return columns_or.status();
        }
        plan.visible_columns = std::move(*columns_or);
        plan.source_columns = plan.visible_columns;
        return plan;
    }
    if (node->inputs.size() != 1) {
        return absl::InvalidArgumentError("non-linear plan is not pushable");
    }
    auto plan_or = build_pushdown_plan(node->inputs[0]);
    if (!plan_or.ok()) {
        return plan_or.status();
    }
    switch (node->kind) {
        case plan::PlanNodeKind::Range: {
            auto source_column_or = source_column_for_visible(
                plan_or->visible_columns, plan_or->source_columns, "_time", "range");
            if (!source_column_or.ok()) {
                return source_column_or.status();
            }
            if (*source_column_or != "_time") {
                return absl::InvalidArgumentError(
                    "range pushdown requires visible _time to map to source _time");
            }
            plan_or->request.time_range = connector::TimeRange{
                .start = node->range.start,
                .stop = node->range.stop,
            };
            return plan_or;
        }
        case plan::PlanNodeKind::Filter:
            if (node->filter.predicates.empty()) {
                return absl::InvalidArgumentError("filter has no pushable predicates");
            }
            for (const auto& predicate : node->filter.predicates) {
                auto source_column_or = source_column_for_visible(
                    plan_or->visible_columns, plan_or->source_columns, predicate.column, "filter");
                if (!source_column_or.ok()) {
                    return source_column_or.status();
                }
                plan_or->request.predicates.push_back({
                    .op = to_connector_predicate_op(predicate.op),
                    .column = *source_column_or,
                    .literal = to_connector_literal(predicate.literal),
                });
            }
            return plan_or;
        case plan::PlanNodeKind::Project: {
            std::vector<std::string> projected_source_columns;
            projected_source_columns.reserve(node->project.columns.size());
            for (const auto& column : node->project.columns) {
                auto source_column_or = source_column_for_visible(
                    plan_or->visible_columns, plan_or->source_columns, column, "project");
                if (!source_column_or.ok()) {
                    return source_column_or.status();
                }
                projected_source_columns.push_back(*source_column_or);
            }
            set_projection_columns(&plan_or->request, projected_source_columns,
                                   node->project.columns);
            plan_or->visible_columns = node->project.columns;
            plan_or->source_columns = std::move(projected_source_columns);
            return plan_or;
        }
        case plan::PlanNodeKind::Rename:
            for (auto& column : plan_or->visible_columns) {
                if (auto renamed = mapped_column_name(node->rename.columns, column);
                    renamed.has_value()) {
                    column = *renamed;
                }
            }
            if (has_duplicate_columns(plan_or->visible_columns)) {
                return absl::InvalidArgumentError("rename produces duplicate columns");
            }
            set_projection_columns(&plan_or->request, plan_or->source_columns,
                                   plan_or->visible_columns);
            return plan_or;
        case plan::PlanNodeKind::Group:
            plan_or->request.group_by.clear();
            plan_or->request.group_by.reserve(node->group.columns.size());
            for (const auto& column : node->group.columns) {
                auto source_column_or = source_column_for_visible(
                    plan_or->visible_columns, plan_or->source_columns, column, "group");
                if (!source_column_or.ok()) {
                    return source_column_or.status();
                }
                plan_or->request.group_by.push_back(*source_column_or);
            }
            return plan_or;
        case plan::PlanNodeKind::Aggregate: {
            if (plan_or->request.distinct.has_value()) {
                return absl::InvalidArgumentError("aggregate after distinct is not pushable");
            }
            auto source_column_or =
                source_column_for_visible(plan_or->visible_columns, plan_or->source_columns,
                                          node->aggregate.column, "aggregate");
            if (!source_column_or.ok()) {
                return source_column_or.status();
            }
            plan_or->request.aggregate = connector::AggregateRequest{
                .fn = to_connector_aggregate_fn(node->aggregate.fn),
                .column = *source_column_or,
                .alias = node->aggregate.column,
            };
            plan_or->request.order_by.clear();
            plan_or->request.limit.reset();
            plan_or->request.offset.reset();
            return plan_or;
        }
        case plan::PlanNodeKind::Distinct: {
            auto source_column_or =
                source_column_for_visible(plan_or->visible_columns, plan_or->source_columns,
                                          node->distinct.column, "distinct");
            if (!source_column_or.ok()) {
                return source_column_or.status();
            }
            plan_or->request.distinct = *source_column_or;
            return plan_or;
        }
        case plan::PlanNodeKind::Limit:
            plan_or->request.limit = node->limit.n;
            if (node->limit.offset != 0) {
                plan_or->request.offset = node->limit.offset;
            }
            return plan_or;
        case plan::PlanNodeKind::Sort:
            plan_or->request.order_by.clear();
            plan_or->request.order_by.reserve(node->sort.keys.size());
            for (const auto& key : node->sort.keys) {
                auto source_column_or = source_column_for_visible(
                    plan_or->visible_columns, plan_or->source_columns, key.column, "sort");
                if (!source_column_or.ok()) {
                    return source_column_or.status();
                }
                plan_or->request.order_by.push_back({
                    .column = *source_column_or,
                    .desc = key.desc,
                });
            }
            return plan_or;
        default:
            return absl::InvalidArgumentError("plan node is not pushable");
    }
}

} // namespace

absl::StatusOr<PushdownPlan> BuildPushdownPlan(const std::shared_ptr<plan::PlanNode>& node) {
    auto optimized_or = DefaultRuleBasedOptimizer().Optimize(node);
    if (!optimized_or.ok()) {
        return optimized_or.status();
    }
    return build_pushdown_plan(optimized_or->plan);
}

bool CanExecutePushdownPlan(const PushdownPlan& plan) {
    return plan.request.group_by.empty() || plan.request.aggregate.has_value();
}

std::string FormatPushdownRequest(const connector::ScanRequest& request) {
    std::ostringstream out;
    out << "SourcePushdown(request: ";
    bool needs_separator = false;

    append_pushdown_summary_field(&out, &needs_separator, "projection",
                                  projection_summary(request));

    if (request.time_range.has_value()) {
        std::ostringstream range;
        range << "{";
        bool has_range_field = false;
        if (request.time_range->start.has_value()) {
            range << "start=" << *request.time_range->start;
            has_range_field = true;
        }
        if (request.time_range->stop.has_value()) {
            if (has_range_field) {
                range << ", ";
            }
            range << "stop=" << *request.time_range->stop;
        }
        range << "}";
        append_pushdown_summary_field(&out, &needs_separator, "time_range", range.str());
    }

    if (!request.predicates.empty()) {
        std::ostringstream predicates;
        predicates << "[";
        for (size_t i = 0; i < request.predicates.size(); ++i) {
            if (i != 0) {
                predicates << ", ";
            }
            const auto& predicate = request.predicates[i];
            predicates << predicate.column << " " << connector_predicate_op_string(predicate.op)
                       << " " << predicate.literal.string();
        }
        predicates << "]";
        append_pushdown_summary_field(&out, &needs_separator, "predicates", predicates.str());
    }

    if (request.distinct.has_value()) {
        append_pushdown_summary_field(&out, &needs_separator, "distinct", *request.distinct);
    }

    if (!request.group_by.empty()) {
        std::ostringstream group_by;
        append_string_list(&group_by, request.group_by);
        append_pushdown_summary_field(&out, &needs_separator, "group_by", group_by.str());
    }

    if (request.aggregate.has_value()) {
        const auto& aggregate = *request.aggregate;
        std::ostringstream value;
        value << connector_aggregate_fn_string(aggregate.fn) << "(" << aggregate.column << ")";
        if (!aggregate.alias.empty() && aggregate.alias != aggregate.column) {
            value << " AS " << aggregate.alias;
        }
        append_pushdown_summary_field(&out, &needs_separator, "aggregate", value.str());
    }

    if (!request.order_by.empty()) {
        std::ostringstream order_by;
        order_by << "[";
        for (size_t i = 0; i < request.order_by.size(); ++i) {
            if (i != 0) {
                order_by << ", ";
            }
            order_by << request.order_by[i].column << (request.order_by[i].desc ? " DESC" : " ASC");
        }
        order_by << "]";
        append_pushdown_summary_field(&out, &needs_separator, "order_by", order_by.str());
    }

    if (request.limit.has_value()) {
        append_pushdown_summary_field(&out, &needs_separator, "limit",
                                      std::to_string(*request.limit));
    }
    if (request.offset.has_value()) {
        append_pushdown_summary_field(&out, &needs_separator, "offset",
                                      std::to_string(*request.offset));
    }

    out << ")";
    return out.str();
}

std::optional<std::string> SourcePushdownSummary(const std::shared_ptr<plan::PlanNode>& plan) {
    auto pushdown_or = BuildPushdownPlan(plan);
    if (!pushdown_or.ok() || !CanExecutePushdownPlan(*pushdown_or)) {
        return std::nullopt;
    }
    return FormatPushdownRequest(pushdown_or->request);
}

absl::StatusOr<Value> ExecutePushdownPlan(const PushdownPlan& plan) {
    if (plan.source == nullptr) {
        return absl::InvalidArgumentError("pushdown plan has no source");
    }
    if (!CanExecutePushdownPlan(plan)) {
        return absl::InvalidArgumentError("group without aggregate is not executable pushdown");
    }
    if (plan.source->source == "sqlite" && plan.source->driver == "sqlite") {
        connector::SQLiteSource source(plan.source->dsn, plan.source->table);
        return source.Scan(plan.request);
    }
    if (plan.source->source == "mysql" && plan.source->driver == "mysql") {
        connector::MySQLSource source(plan.source->dsn, plan.source->table);
        return source.Scan(plan.request);
    }
    return absl::InvalidArgumentError("unsupported pushdown source");
}

Value MaybeExecutePushedSourcePlan(Value value) {
    auto& table = value.as_table_mut();
    if (table.plan == nullptr) {
        return value;
    }
    auto pushdown_or = BuildPushdownPlan(table.plan);
    if (!pushdown_or.ok() || !CanExecutePushdownPlan(*pushdown_or)) {
        return value;
    }
    auto pushed_or = ExecutePushdownPlan(*pushdown_or);
    if (!pushed_or.ok()) {
        return value;
    }
    pushed_or->as_table_mut().plan = table.plan;
    pushed_or->as_table_mut().range_start = table.range_start;
    pushed_or->as_table_mut().range_stop = table.range_stop;
    pushed_or->as_table_mut().result_name = table.result_name;
    return *pushed_or;
}

} // namespace pl::flux::optimizer
