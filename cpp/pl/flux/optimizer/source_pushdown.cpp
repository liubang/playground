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
#include "cpp/pl/flux/connector/mysql_source.h"
#include "cpp/pl/flux/connector/sqlite_source.h"
#include <sstream>

namespace pl::flux::optimizer {
namespace {

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

} // namespace

absl::StatusOr<PushdownPlan> BuildPushdownPlan(const std::shared_ptr<plan::PlanNode>& node) {
    auto optimized_or = DefaultRuleBasedOptimizer().Optimize(node);
    if (!optimized_or.ok()) {
        return optimized_or.status();
    }
    if (!optimized_or->pushdown_plan.has_value()) {
        return absl::InvalidArgumentError("plan is not pushable");
    }
    return *optimized_or->pushdown_plan;
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
    auto optimized_or = DefaultRuleBasedOptimizer().Optimize(plan);
    if (!optimized_or.ok() || !optimized_or->pushdown_plan.has_value() ||
        !CanExecutePushdownPlan(*optimized_or->pushdown_plan)) {
        return std::nullopt;
    }
    return FormatPushdownRequest(optimized_or->pushdown_plan->request);
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

} // namespace pl::flux::optimizer
