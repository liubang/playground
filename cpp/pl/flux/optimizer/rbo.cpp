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

#include "cpp/pl/flux/optimizer/rbo.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/connector_registry.h"
#include "cpp/pl/flux/connector/connector_runtime.h"
#include "cpp/pl/flux/runtime/runtime_value.h"
#include <algorithm>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>

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

absl::StatusOr<PushdownPlan> build_pushdown_plan(const std::shared_ptr<plan::PlanNode>& node) {
    if (node == nullptr) {
        return absl::InvalidArgumentError("missing plan");
    }
    if (node->kind == plan::PlanNodeKind::SourceScan) {
        if (!IsPushdownSourceScan(*node)) {
            return absl::InvalidArgumentError("unsupported pushdown source");
        }
        PushdownPlan plan;
        plan.source = &node->source_scan();
        auto columns_or = SourceScanColumns(node->source_scan());
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
                .start = node->range().start,
                .stop = node->range().stop,
            };
            return plan_or;
        }
        case plan::PlanNodeKind::Filter:
            if (node->filter().predicates.empty()) {
                return absl::InvalidArgumentError("filter has no pushable predicates");
            }
            for (const auto& predicate : node->filter().predicates) {
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
            projected_source_columns.reserve(node->project().columns.size());
            for (const auto& column : node->project().columns) {
                auto source_column_or = source_column_for_visible(
                    plan_or->visible_columns, plan_or->source_columns, column, "project");
                if (!source_column_or.ok()) {
                    return source_column_or.status();
                }
                projected_source_columns.push_back(*source_column_or);
            }
            set_projection_columns(&plan_or->request, projected_source_columns,
                                   node->project().columns);
            plan_or->visible_columns = node->project().columns;
            plan_or->source_columns = std::move(projected_source_columns);
            return plan_or;
        }
        case plan::PlanNodeKind::Rename:
            for (auto& column : plan_or->visible_columns) {
                if (auto renamed = mapped_column_name(node->rename().columns, column);
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
            plan_or->request.group_by.reserve(node->group().columns.size());
            for (const auto& column : node->group().columns) {
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
                                          node->aggregate().column, "aggregate");
            if (!source_column_or.ok()) {
                return source_column_or.status();
            }
            plan_or->request.aggregate = connector::AggregateRequest{
                .fn = to_connector_aggregate_fn(node->aggregate().fn),
                .column = *source_column_or,
                .alias = node->aggregate().column,
            };
            plan_or->request.order_by.clear();
            plan_or->request.limit.reset();
            plan_or->request.offset.reset();
            return plan_or;
        }
        case plan::PlanNodeKind::Distinct: {
            auto source_column_or =
                source_column_for_visible(plan_or->visible_columns, plan_or->source_columns,
                                          node->distinct().column, "distinct");
            if (!source_column_or.ok()) {
                return source_column_or.status();
            }
            plan_or->request.distinct = *source_column_or;
            return plan_or;
        }
        case plan::PlanNodeKind::Limit:
            plan_or->request.limit = node->limit().n;
            if (node->limit().offset != 0) {
                plan_or->request.offset = node->limit().offset;
            }
            return plan_or;
        case plan::PlanNodeKind::Sort:
            plan_or->request.order_by.clear();
            plan_or->request.order_by.reserve(node->sort().keys.size());
            for (const auto& key : node->sort().keys) {
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

bool prefix_contains_node_kind(const std::shared_ptr<plan::PlanNode>& node,
                               plan::PlanNodeKind kind) {
    if (node == nullptr) {
        return false;
    }
    if (node->kind == kind) {
        return true;
    }
    if (!IsPushableUnaryNode(*node) || node->inputs.size() != 1) {
        return false;
    }
    return prefix_contains_node_kind(node->inputs[0], kind);
}

bool prefix_contains_any_node_kind(const std::shared_ptr<plan::PlanNode>& node,
                                   const std::vector<plan::PlanNodeKind>& kinds) {
    return std::any_of(kinds.begin(), kinds.end(), [&](plan::PlanNodeKind kind) {
        return prefix_contains_node_kind(node, kind);
    });
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

void append_pushdown_scalar_field(std::ostringstream* out,
                                  const std::string& name,
                                  const std::string& value) {
    *out << "    " << name << ": " << value << "\n";
}

void append_pushdown_list_field(std::ostringstream* out,
                                const std::string& name,
                                const std::vector<std::string>& values) {
    if (values.empty()) {
        return;
    }
    *out << "    " << name << ":\n";
    for (const auto& value : values) {
        *out << "      - " << value << "\n";
    }
}

class PushdownDetectionRule final : public Rule {
public:
    PushdownDetectionRule(std::string name, std::vector<plan::PlanNodeKind> kinds)
        : name_(std::move(name)), kinds_(std::move(kinds)) {}

    [[nodiscard]] std::string Name() const override { return name_; }

    [[nodiscard]] absl::StatusOr<RuleApplication> Apply(
        const std::shared_ptr<plan::PlanNode>& node) const override {
        if (node == nullptr) {
            return absl::InvalidArgumentError("missing plan");
        }
        return RuleApplication{
            .plan = node,
            .applied = prefix_contains_any_node_kind(node, kinds_),
            .detail = "",
        };
    }

private:
    std::string name_;
    std::vector<plan::PlanNodeKind> kinds_;
};

bool insert_materialization_barriers(const std::shared_ptr<plan::PlanNode>& node,
                                     std::shared_ptr<plan::PlanNode>* rewritten) {
    if (node == nullptr) {
        *rewritten = node;
        return false;
    }
    if (node->kind == plan::PlanNodeKind::SourceScan) {
        *rewritten = node;
        return false;
    }

    bool changed = false;
    std::vector<std::shared_ptr<plan::PlanNode>> rewritten_inputs;
    rewritten_inputs.reserve(node->inputs.size());
    for (const auto& input : node->inputs) {
        std::shared_ptr<plan::PlanNode> next_input;
        changed = insert_materialization_barriers(input, &next_input) || changed;
        rewritten_inputs.push_back(std::move(next_input));
    }

    auto next = changed ? std::make_shared<plan::PlanNode>(*node) : node;
    if (changed) {
        next->inputs = std::move(rewritten_inputs);
    }

    if (node->kind == plan::PlanNodeKind::Materialize) {
        *rewritten = next;
        return true;
    }
    if (node->inputs.size() == 1 && node->inputs[0] != nullptr && !IsPushableUnaryNode(*node)) {
        const PushdownState input_state = AnalyzePushdownState(*node->inputs[0]);
        if (input_state == PushdownState::SourceScan ||
            input_state == PushdownState::SourcePushdown) {
            if (next == node) {
                next = std::make_shared<plan::PlanNode>(*node);
            }
            next->inputs[0] = plan::MakeMaterializeBarrier(
                node->inputs[0], "unsupported lazy builtin", plan::PlanNodeKindName(node->kind));
            *rewritten = std::move(next);
            return true;
        }
    }

    *rewritten = std::move(next);
    return changed;
}

class InsertMaterializationBarrierRule final : public Rule {
public:
    [[nodiscard]] std::string Name() const override { return "InsertMaterializationBarrier"; }

    [[nodiscard]] absl::StatusOr<RuleApplication> Apply(
        const std::shared_ptr<plan::PlanNode>& node) const override {
        if (node == nullptr) {
            return absl::InvalidArgumentError("missing plan");
        }
        std::shared_ptr<plan::PlanNode> rewritten;
        const bool applied = insert_materialization_barriers(node, &rewritten);
        return RuleApplication{
            .plan = std::move(rewritten),
            .applied = applied,
            .detail = "",
        };
    }
};

std::unique_ptr<Rule> MakePushdownDetectionRule(std::string name,
                                                std::vector<plan::PlanNodeKind> kinds) {
    return std::unique_ptr<Rule>(new PushdownDetectionRule(std::move(name), std::move(kinds)));
}

} // namespace

RuleBasedOptimizer::RuleBasedOptimizer(std::vector<std::unique_ptr<Rule>> rules)
    : rules_(std::move(rules)) {}

absl::StatusOr<PlanOptimizerResult> RuleBasedOptimizer::Optimize(
    std::shared_ptr<plan::PlanNode> plan) const {
    if (plan == nullptr) {
        return absl::InvalidArgumentError("missing plan");
    }
    PlanOptimizerResult result;
    result.plan = std::move(plan);
    result.trace.reserve(rules_.size());
    for (const auto& rule : rules_) {
        auto application_or = rule->Apply(result.plan);
        if (!application_or.ok()) {
            return application_or.status();
        }
        result.plan = std::move(application_or->plan);
        result.trace.push_back({
            .rule = rule->Name(),
            .applied = application_or->applied,
            .detail = std::move(application_or->detail),
        });
    }
    auto pushdown_or = build_pushdown_plan(result.plan);
    if (pushdown_or.ok()) {
        result.pushdown_plan = std::move(*pushdown_or);
    }
    return result;
}

RuleBasedOptimizer DefaultRuleBasedOptimizer() {
    std::vector<std::unique_ptr<Rule>> rules;
    rules.push_back(
        MakePushdownDetectionRule("PushLimitIntoConnectorScan", {plan::PlanNodeKind::Limit}));
    rules.push_back(
        MakePushdownDetectionRule("PushSortIntoConnectorScan", {plan::PlanNodeKind::Sort}));
    rules.push_back(
        MakePushdownDetectionRule("PushProjectionIntoConnectorScan",
                                  {plan::PlanNodeKind::Project, plan::PlanNodeKind::Rename}));
    rules.push_back(
        MakePushdownDetectionRule("PushPredicateIntoConnectorScan", {plan::PlanNodeKind::Filter}));
    rules.push_back(
        MakePushdownDetectionRule("PushTimeRangeIntoConnectorScan", {plan::PlanNodeKind::Range}));
    rules.push_back(
        MakePushdownDetectionRule("PushDistinctIntoConnectorScan", {plan::PlanNodeKind::Distinct}));
    rules.push_back(
        MakePushdownDetectionRule("PushAggregateIntoConnectorScan",
                                  {plan::PlanNodeKind::Group, plan::PlanNodeKind::Aggregate}));
    rules.push_back(std::unique_ptr<Rule>(new InsertMaterializationBarrierRule()));
    return RuleBasedOptimizer(std::move(rules));
}

std::vector<std::string> AppliedRuleNames(const PlanOptimizerResult& result) {
    std::vector<std::string> rules;
    for (const auto& trace : result.trace) {
        if (trace.applied) {
            rules.push_back(trace.rule);
        }
    }
    return rules;
}

absl::StatusOr<std::vector<std::string>> SourceScanColumns(const plan::SourceScanSpec& source) {
    connector::SourceSpec spec{
        .source = source.source, .driver = source.driver, .dsn = source.dsn, .table = source.table};
    auto runtime_or = connector::ConnectorRegistry::Global().CreateRuntime(spec);
    if (!runtime_or.ok()) {
        return runtime_or.status();
    }
    auto handle_or = (*runtime_or)->metadata->GetTableHandle(spec);
    if (!handle_or.ok()) {
        return handle_or.status();
    }
    auto schema_or = (*runtime_or)->metadata->Schema(*handle_or);
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
        return SourceScanColumns(node->source_scan());
    }
    if (node->inputs.size() != 1) {
        return absl::InvalidArgumentError("non-linear plan has no stable visible columns");
    }
    auto columns_or = VisibleColumnsForPlan(node->inputs[0]);
    if (!columns_or.ok()) {
        return columns_or.status();
    }
    if (node->kind == plan::PlanNodeKind::Project) {
        return node->project().columns;
    }
    if (node->kind == plan::PlanNodeKind::Rename) {
        for (auto& column : *columns_or) {
            if (auto renamed = mapped_column_name(node->rename().columns, column);
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
        node->kind == plan::PlanNodeKind::Distinct ||
        node->kind == plan::PlanNodeKind::Exchange ||
        node->kind == plan::PlanNodeKind::Materialize) {
        return columns_or;
    }
    if (node->kind == plan::PlanNodeKind::Join) {
        return absl::InvalidArgumentError("join output columns depend on both inputs");
    }
    return absl::InvalidArgumentError("plan node has no stable visible columns");
}

bool CanExecutePushdownPlan(const PushdownPlan& plan) {
    return plan.request.group_by.empty() || plan.request.aggregate.has_value();
}

std::string FormatPushdownRequest(const connector::ScanRequest& request) {
    std::ostringstream out;
    out << "SourcePushdown\n";
    out << "  request:\n";

    append_pushdown_scalar_field(&out, "projection", projection_summary(request));

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
        append_pushdown_scalar_field(&out, "time_range", range.str());
    }

    if (!request.predicates.empty()) {
        std::vector<std::string> predicates;
        predicates.reserve(request.predicates.size());
        for (size_t i = 0; i < request.predicates.size(); ++i) {
            const auto& predicate = request.predicates[i];
            std::ostringstream predicate_out;
            predicate_out << predicate.column << " " << connector_predicate_op_string(predicate.op)
                          << " " << predicate.literal.string();
            predicates.push_back(predicate_out.str());
        }
        append_pushdown_list_field(&out, "predicates", predicates);
    }

    if (request.distinct.has_value()) {
        append_pushdown_scalar_field(&out, "distinct", *request.distinct);
    }

    if (!request.group_by.empty()) {
        std::ostringstream group_by;
        append_string_list(&group_by, request.group_by);
        append_pushdown_scalar_field(&out, "group_by", group_by.str());
    }

    if (request.aggregate.has_value()) {
        const auto& aggregate = *request.aggregate;
        std::ostringstream value;
        value << connector_aggregate_fn_string(aggregate.fn) << "(" << aggregate.column << ")";
        if (!aggregate.alias.empty() && aggregate.alias != aggregate.column) {
            value << " AS " << aggregate.alias;
        }
        append_pushdown_scalar_field(&out, "aggregate", value.str());
    }

    if (!request.order_by.empty()) {
        std::vector<std::string> order_by;
        order_by.reserve(request.order_by.size());
        for (size_t i = 0; i < request.order_by.size(); ++i) {
            order_by.push_back(request.order_by[i].column +
                               (request.order_by[i].desc ? " DESC" : " ASC"));
        }
        append_pushdown_list_field(&out, "order_by", order_by);
    }

    if (request.limit.has_value()) {
        append_pushdown_scalar_field(&out, "limit", std::to_string(*request.limit));
    }
    if (request.offset.has_value()) {
        append_pushdown_scalar_field(&out, "offset", std::to_string(*request.offset));
    }

    return out.str();
}

std::optional<std::string> SourcePushdownSummary(const PlanOptimizerResult& result) {
    if (!result.pushdown_plan.has_value() || !CanExecutePushdownPlan(*result.pushdown_plan)) {
        return std::nullopt;
    }
    return FormatPushdownRequest(result.pushdown_plan->request);
}

bool IsPushdownSourceScan(const plan::PlanNode& node) {
    if (node.kind != plan::PlanNodeKind::SourceScan) {
        return false;
    }
    return connector::IsSqlPushdownConnector(node.source_scan().source, node.source_scan().driver);
}

bool IsPushableUnaryNode(const plan::PlanNode& node) {
    switch (node.kind) {
        case plan::PlanNodeKind::Range:
        case plan::PlanNodeKind::Project:
        case plan::PlanNodeKind::Rename:
        case plan::PlanNodeKind::Limit:
        case plan::PlanNodeKind::Sort:
        case plan::PlanNodeKind::Group:
        case plan::PlanNodeKind::Aggregate:
        case plan::PlanNodeKind::Distinct:
        case plan::PlanNodeKind::Exchange:
            return true;
        case plan::PlanNodeKind::Filter:
            return !node.filter().predicates.empty();
        default:
            return false;
    }
}

PushdownState AnalyzePushdownState(const plan::PlanNode& node) {
    if (IsPushdownSourceScan(node)) {
        return PushdownState::SourceScan;
    }
    if (node.kind == plan::PlanNodeKind::Materialize) {
        return PushdownState::MaterializeBarrier;
    }
    if (!IsPushableUnaryNode(node) || node.inputs.size() != 1 || node.inputs[0] == nullptr) {
        return PushdownState::Memory;
    }
    const PushdownState input_state = AnalyzePushdownState(*node.inputs[0]);
    if (input_state == PushdownState::SourceScan || input_state == PushdownState::SourcePushdown) {
        return PushdownState::SourcePushdown;
    }
    return PushdownState::Memory;
}

std::optional<std::string> PushdownSourceName(const plan::PlanNode& node) {
    if (IsPushdownSourceScan(node)) {
        return node.source_scan().source;
    }
    if (!IsPushableUnaryNode(node) || node.inputs.size() != 1 || node.inputs[0] == nullptr) {
        return std::nullopt;
    }
    return PushdownSourceName(*node.inputs[0]);
}

bool ContainsPlanNodeKind(const plan::PlanNode& node, plan::PlanNodeKind kind) {
    if (node.kind == kind) {
        return true;
    }
    for (const auto& input : node.inputs) {
        if (input != nullptr && ContainsPlanNodeKind(*input, kind)) {
            return true;
        }
    }
    return false;
}

bool IsExecutableConnectorPrefix(const plan::PlanNode& node) {
    if (!ContainsPlanNodeKind(node, plan::PlanNodeKind::Group)) {
        return true;
    }
    return ContainsPlanNodeKind(node, plan::PlanNodeKind::Aggregate);
}

} // namespace pl::flux::optimizer
