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

#include "cpp/pl/flux/optimizer/cbo.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/connector_registry.h"
#include "cpp/pl/flux/connector/connector_runtime.h"

namespace pl::flux::optimizer {
namespace {

constexpr double kRangeSelectivity = 0.25;
constexpr double kFilterSelectivity = 0.5;
constexpr double kProjectCpuPerRow = 0.05;
constexpr double kMemoryCpuPerRow = 1.0;
constexpr double kConnectorCpuPerRow = 0.05;
constexpr double kMaterializeCpuPerRow = 0.1;
constexpr double kJoinBuildCpuPerRow = 1.25;
constexpr double kJoinProbeCpuPerRow = 0.75;

bool has_rows(const plan::CostEstimate& cost) {
    return cost.rows.has_value();
}

struct EstimatedPlan {
    plan::CostEstimate cost;
    std::vector<connector::ColumnStatistics> columns{};
};

enum class ExecutionLocation {
    Memory,
    Connector,
};

plan::CostEstimate unknown_cost() {
    return {};
}

EstimatedPlan unknown_plan() {
    return {};
}

std::shared_ptr<plan::PlanNode> clone_plan(const std::shared_ptr<plan::PlanNode>& node) {
    if (node == nullptr) {
        return nullptr;
    }
    auto clone = std::make_shared<plan::PlanNode>(*node);
    clone->inputs.clear();
    clone->inputs.reserve(node->inputs.size());
    for (const auto& input : node->inputs) {
        clone->inputs.push_back(clone_plan(input));
    }
    return clone;
}

double safe_non_negative(int64_t value) {
    return static_cast<double>(std::max<int64_t>(0, value));
}

double clamp_fraction(double fraction) {
    return std::clamp(fraction, 0.0, 1.0);
}

const connector::ColumnStatistics* find_column(
    const std::vector<connector::ColumnStatistics>& columns, const std::string& name) {
    auto it =
        std::ranges::find_if(columns, [&](const auto& column) { return column.name == name; });
    return it == columns.end() ? nullptr : &*it;
}

connector::ColumnStatistics* find_column(std::vector<connector::ColumnStatistics>* columns,
                                         const std::string& name) {
    auto it =
        std::ranges::find_if(*columns, [&](const auto& column) { return column.name == name; });
    return it == columns->end() ? nullptr : &*it;
}

double non_null_fraction(const connector::ColumnStatistics* column) {
    return column == nullptr ? 1.0 : 1.0 - clamp_fraction(column->null_fraction.value_or(0.0));
}

double distinct_values_with_null(const connector::ColumnStatistics& column) {
    return std::max(0.0, column.distinct_values.value_or(0.0)) +
           (column.null_fraction.value_or(0.0) > 0.0 ? 1.0 : 0.0);
}

void cap_distinct_values(std::vector<connector::ColumnStatistics>* columns, double row_count) {
    for (auto& column : *columns) {
        if (column.distinct_values.has_value()) {
            column.distinct_values = std::min(std::max(0.0, *column.distinct_values), row_count);
        }
    }
}

double predicate_selectivity(const plan::PredicateSpec& predicate,
                             const std::vector<connector::ColumnStatistics>& columns) {
    const auto* column = find_column(columns, predicate.column);
    const double non_null = non_null_fraction(column);
    const auto distinct_values = column == nullptr ? std::nullopt : column->distinct_values;
    if (predicate.op == plan::PredicateOp::Eq) {
        if (distinct_values.has_value()) {
            return *distinct_values <= 0.0 ? 0.0 : clamp_fraction(non_null / *distinct_values);
        }
        return kFilterSelectivity * non_null;
    }
    if (predicate.op == plan::PredicateOp::NotEq) {
        if (distinct_values.has_value()) {
            return *distinct_values <= 0.0
                       ? 0.0
                       : clamp_fraction(non_null * (1.0 - 1.0 / *distinct_values));
        }
        return kFilterSelectivity * non_null;
    }
    return kRangeSelectivity * non_null;
}

std::vector<connector::ColumnStatistics> filter_columns(
    std::vector<connector::ColumnStatistics> columns,
    const std::vector<plan::PredicateSpec>& predicates,
    double output_rows) {
    cap_distinct_values(&columns, output_rows);
    for (const auto& predicate : predicates) {
        auto* column = find_column(&columns, predicate.column);
        if (column == nullptr) {
            continue;
        }
        column->null_fraction = 0.0;
        if (predicate.op == plan::PredicateOp::Eq) {
            column->distinct_values = output_rows > 0.0 ? 1.0 : 0.0;
        }
    }
    return columns;
}

std::vector<connector::ColumnStatistics> project_columns(
    const std::vector<connector::ColumnStatistics>& columns,
    const std::vector<std::string>& projected_names) {
    std::vector<connector::ColumnStatistics> projected;
    projected.reserve(projected_names.size());
    for (const auto& name : projected_names) {
        if (const auto* column = find_column(columns, name); column != nullptr) {
            projected.push_back(*column);
        } else {
            projected.push_back({.name = name});
        }
    }
    return projected;
}

std::vector<connector::ColumnStatistics> rename_columns(
    std::vector<connector::ColumnStatistics> columns,
    const std::vector<std::pair<std::string, std::string>>& mappings) {
    for (auto& column : columns) {
        for (const auto& [from, to] : mappings) {
            if (column.name == from) {
                column.name = to;
                break;
            }
        }
    }
    return columns;
}

std::optional<double> estimate_cardinality(const std::vector<connector::ColumnStatistics>& columns,
                                           const std::vector<std::string>& names,
                                           double input_rows) {
    double cardinality = 1.0;
    for (const auto& name : names) {
        const auto* column = find_column(columns, name);
        if (column == nullptr || !column->distinct_values.has_value()) {
            return std::nullopt;
        }
        cardinality *= distinct_values_with_null(*column);
        if (cardinality >= input_rows) {
            return input_rows;
        }
    }
    return std::min(input_rows, cardinality);
}

std::optional<double> estimate_inner_join_rows(
    const std::vector<connector::ColumnStatistics>& left_columns,
    const std::vector<connector::ColumnStatistics>& right_columns,
    const std::vector<std::string>& join_keys,
    double left_rows,
    double right_rows) {
    if (join_keys.empty()) {
        return std::nullopt;
    }
    double rows = left_rows * right_rows;
    for (const auto& key : join_keys) {
        const auto* left = find_column(left_columns, key);
        const auto* right = find_column(right_columns, key);
        if (left == nullptr || right == nullptr || !left->distinct_values.has_value() ||
            !right->distinct_values.has_value()) {
            return std::nullopt;
        }
        const double max_distinct = std::max(*left->distinct_values, *right->distinct_values);
        if (max_distinct <= 0.0) {
            return 0.0;
        }
        rows *= non_null_fraction(left) * non_null_fraction(right) / max_distinct;
    }
    return std::max(0.0, rows);
}

double estimate_join_rows(const EstimatedPlan& left,
                          const EstimatedPlan& right,
                          const plan::JoinSpec& join,
                          double left_rows,
                          double right_rows) {
    const double inner_rows =
        estimate_inner_join_rows(left.columns, right.columns, join.on, left_rows, right_rows)
            .value_or(std::min(left_rows, right_rows));
    switch (join.method) {
        case plan::JoinMethod::Inner:
            return inner_rows;
        case plan::JoinMethod::Left:
            return std::max(left_rows, inner_rows);
        case plan::JoinMethod::Right:
            return std::max(right_rows, inner_rows);
        case plan::JoinMethod::Full:
            return std::max({left_rows, right_rows, inner_rows});
    }
    return inner_rows;
}

EstimatedPlan combine_unary_cost(const EstimatedPlan& input,
                                 double output_rows,
                                 double extra_cpu,
                                 std::vector<connector::ColumnStatistics> columns) {
    cap_distinct_values(&columns, output_rows);
    return EstimatedPlan{
        .cost =
            {
                .rows = output_rows,
                .cpu = input.cost.cpu.value_or(0.0) + extra_cpu,
                .io = input.cost.io.value_or(0.0),
            },
        .columns = std::move(columns),
    };
}

class ConnectorStatsProvider final : public StatsProvider {
public:
    explicit ConnectorStatsProvider(CboOptions options) : options_(options) {}

    [[nodiscard]] absl::StatusOr<PlanStatistics> Estimate(
        const std::shared_ptr<plan::PlanNode>& node) const override {
        if (node == nullptr) {
            return absl::InvalidArgumentError("missing plan");
        }
        if (node->kind != plan::PlanNodeKind::SourceScan || !options_.collect_connector_stats) {
            return PlanStatistics{};
        }

        const auto& source = node->source_scan();
        connector::SourceSpec spec{.source = source.source,
                                   .driver = source.driver,
                                   .dsn = source.dsn,
                                   .table = source.table};
        auto runtime_or = connector::ConnectorRegistry::Global().CreateRuntime(spec);
        if (!runtime_or.ok()) {
            return PlanStatistics{};
        }
        auto handle_or = (*runtime_or)->metadata->GetTableHandle(spec);
        if (!handle_or.ok()) {
            return PlanStatistics{};
        }
        auto statistics_or = (*runtime_or)->metadata->Statistics(*handle_or);
        if (!statistics_or.ok()) {
            return PlanStatistics{};
        }
        return PlanStatistics{
            .row_count = statistics_or->row_count,
            .size_bytes = statistics_or->size_bytes,
            .columns = statistics_or->columns,
        };
    }

private:
    CboOptions options_;
};

class HeuristicCostModel final : public CostModel {
public:
    [[nodiscard]] absl::StatusOr<plan::CostEstimate> EstimateCost(
        const std::shared_ptr<plan::PlanNode>& node,
        const StatsProvider& stats_provider) const override {
        if (node == nullptr) {
            return absl::InvalidArgumentError("missing plan");
        }
        auto estimated_or = estimate(node, stats_provider, ExecutionLocation::Memory);
        if (!estimated_or.ok()) {
            return estimated_or.status();
        }
        return estimated_or->cost;
    }

    [[nodiscard]] absl::StatusOr<plan::CostEstimate> EstimateConnectorCost(
        const std::shared_ptr<plan::PlanNode>& node, const StatsProvider& stats_provider) const {
        if (node == nullptr) {
            return absl::InvalidArgumentError("missing plan");
        }
        auto estimated_or = estimate(node, stats_provider, ExecutionLocation::Connector);
        if (!estimated_or.ok()) {
            return estimated_or.status();
        }
        return estimated_or->cost;
    }

private:
    [[nodiscard]] absl::StatusOr<EstimatedPlan> estimate(
        const std::shared_ptr<plan::PlanNode>& node,
        const StatsProvider& stats_provider,
        ExecutionLocation location) const {
        if (node->kind == plan::PlanNodeKind::SourceScan) {
            auto stats_or = stats_provider.Estimate(node);
            if (!stats_or.ok() || !stats_or->row_count.has_value()) {
                return unknown_plan();
            }
            const double rows = *stats_or->row_count;
            return EstimatedPlan{
                .cost =
                    {
                        .rows = rows,
                        .cpu = rows * kConnectorCpuPerRow,
                        .io = stats_or->size_bytes.value_or(rows),
                    },
                .columns = std::move(stats_or->columns),
            };
        }

        if (node->kind == plan::PlanNodeKind::Join) {
            if (node->inputs.size() != 2 || node->inputs[0] == nullptr ||
                node->inputs[1] == nullptr) {
                return unknown_plan();
            }
            auto left_or = estimate(node->inputs[0], stats_provider, ExecutionLocation::Memory);
            if (!left_or.ok()) {
                return left_or.status();
            }
            auto right_or = estimate(node->inputs[1], stats_provider, ExecutionLocation::Memory);
            if (!right_or.ok()) {
                return right_or.status();
            }
            if (!left_or->cost.rows.has_value() || !right_or->cost.rows.has_value()) {
                return unknown_plan();
            }
            const double left_rows = *left_or->cost.rows;
            const double right_rows = *right_or->cost.rows;
            const bool build_left = node->join().build_side == plan::JoinBuildSide::Left;
            const double build_rows = build_left ? left_rows : right_rows;
            const double probe_rows = build_left ? right_rows : left_rows;
            const double output_rows =
                estimate_join_rows(*left_or, *right_or, node->join(), left_rows, right_rows);
            return EstimatedPlan{
                .cost =
                    {
                        .rows = output_rows,
                        .cpu = left_or->cost.cpu.value_or(0.0) + right_or->cost.cpu.value_or(0.0) +
                               build_rows * kJoinBuildCpuPerRow + probe_rows * kJoinProbeCpuPerRow,
                        .io = left_or->cost.io.value_or(0.0) + right_or->cost.io.value_or(0.0),
                    },
            };
        }

        if (node->inputs.size() != 1 || node->inputs[0] == nullptr) {
            return unknown_plan();
        }
        const ExecutionLocation input_location =
            node->kind == plan::PlanNodeKind::Materialize ? ExecutionLocation::Connector : location;
        auto input_or = estimate(node->inputs[0], stats_provider, input_location);
        if (!input_or.ok()) {
            return input_or.status();
        }
        if (!has_rows(input_or->cost)) {
            return unknown_plan();
        }

        const double input_rows = *input_or->cost.rows;
        double output_rows = input_rows;
        double extra_cpu =
            input_rows *
            (location == ExecutionLocation::Connector ? kConnectorCpuPerRow : kMemoryCpuPerRow);
        auto columns = input_or->columns;
        switch (node->kind) {
            case plan::PlanNodeKind::Range: {
                auto* time_column = find_column(&columns, "_time");
                output_rows = input_rows * kRangeSelectivity * non_null_fraction(time_column);
                if (time_column != nullptr) {
                    time_column->null_fraction = 0.0;
                }
                break;
            }
            case plan::PlanNodeKind::Filter: {
                double selectivity = 1.0;
                for (const auto& predicate : node->filter().predicates) {
                    selectivity *= predicate_selectivity(predicate, columns);
                }
                output_rows = input_rows * clamp_fraction(selectivity);
                columns =
                    filter_columns(std::move(columns), node->filter().predicates, output_rows);
                break;
            }
            case plan::PlanNodeKind::Project:
                columns = project_columns(columns, node->project().columns);
                extra_cpu = input_rows * kProjectCpuPerRow;
                break;
            case plan::PlanNodeKind::Rename:
                columns = rename_columns(std::move(columns), node->rename().columns);
                extra_cpu = input_rows * kProjectCpuPerRow;
                break;
            case plan::PlanNodeKind::Limit:
                output_rows = std::min(input_rows, safe_non_negative(node->limit().n));
                break;
            case plan::PlanNodeKind::Sort:
                extra_cpu = input_rows * std::log2(std::max(2.0, input_rows));
                break;
            case plan::PlanNodeKind::Group: {
                const auto cardinality =
                    estimate_cardinality(columns, node->group().columns, input_rows);
                output_rows = cardinality.value_or(
                    std::min(input_rows, std::max(1.0, std::sqrt(input_rows))));
                break;
            }
            case plan::PlanNodeKind::Aggregate:
                output_rows = node->inputs[0]->kind == plan::PlanNodeKind::Group ? input_rows : 1.0;
                break;
            case plan::PlanNodeKind::Distinct: {
                const auto cardinality =
                    estimate_cardinality(columns, {node->distinct().column}, input_rows);
                output_rows = cardinality.value_or(
                    std::min(input_rows, std::max(1.0, std::sqrt(input_rows))));
                columns = project_columns(columns, {node->distinct().column});
                break;
            }
            case plan::PlanNodeKind::Materialize:
                extra_cpu = input_rows * kMaterializeCpuPerRow;
                break;
            case plan::PlanNodeKind::Exchange:
                extra_cpu = input_rows * 0.2;
                break;
            default:
                return unknown_plan();
        }
        return combine_unary_cost(*input_or, output_rows, extra_cpu, std::move(columns));
    }
};

PhysicalShape physical_shape_for(const PlanOptimizerResult& rbo_result) {
    if (rbo_result.pushdown_plan.has_value() && CanExecutePushdownPlan(*rbo_result.pushdown_plan)) {
        return PhysicalShape::ConnectorScan;
    }
    if (rbo_result.plan != nullptr && rbo_result.plan->kind == plan::PlanNodeKind::Join) {
        return PhysicalShape::LocalHashJoin;
    }
    if (rbo_result.plan != nullptr && rbo_result.plan->kind == plan::PlanNodeKind::Exchange) {
        return PhysicalShape::Exchange;
    }
    if (rbo_result.plan != nullptr && rbo_result.plan->inputs.size() == 1) {
        return PhysicalShape::ConnectorPrefixMemorySuffix;
    }
    return PhysicalShape::MemoryScan;
}

std::string alternative_name(PhysicalShape shape) {
    switch (shape) {
        case PhysicalShape::ConnectorScan:
            return "connector_scan";
        case PhysicalShape::ConnectorPrefixMemorySuffix:
            return "connector_prefix_memory_suffix";
        case PhysicalShape::LocalHashJoin:
            return "local_hash_join";
        case PhysicalShape::Exchange:
            return "exchange";
        case PhysicalShape::MemoryScan:
            return "memory_scan";
    }
    return "memory_scan";
}

std::string cbo_decision(const CboOptions& options, const plan::CostEstimate& cost) {
    if (cost.rows.has_value()) {
        return "chosen";
    }
    return options.collect_connector_stats ? "no-stats" : "fallback-rbo";
}

std::string join_alternative_name(plan::JoinBuildSide side) {
    return absl::StrCat("local_hash_join_build_", plan::JoinBuildSideName(side));
}

absl::StatusOr<plan::CostEstimate> EstimateJoinWithBuildSide(
    const std::shared_ptr<plan::PlanNode>& node,
    plan::JoinBuildSide side,
    const HeuristicCostModel& cost_model,
    const ConnectorStatsProvider& stats_provider) {
    if (node == nullptr || node->kind != plan::PlanNodeKind::Join) {
        return unknown_cost();
    }
    auto candidate = clone_plan(node);
    candidate->join().build_side = side;
    return cost_model.EstimateCost(candidate, stats_provider);
}

absl::Status ChooseJoinBuildSide(const std::shared_ptr<plan::PlanNode>& node,
                                 const HeuristicCostModel& cost_model,
                                 const ConnectorStatsProvider& stats_provider) {
    if (node == nullptr || node->kind != plan::PlanNodeKind::Join) {
        return absl::OkStatus();
    }
    auto left_cost_or =
        EstimateJoinWithBuildSide(node, plan::JoinBuildSide::Left, cost_model, stats_provider);
    if (!left_cost_or.ok()) {
        return left_cost_or.status();
    }
    auto right_cost_or =
        EstimateJoinWithBuildSide(node, plan::JoinBuildSide::Right, cost_model, stats_provider);
    if (!right_cost_or.ok()) {
        return right_cost_or.status();
    }
    if (left_cost_or->cpu.has_value() && right_cost_or->cpu.has_value() &&
        *left_cost_or->cpu < *right_cost_or->cpu) {
        node->join().build_side = plan::JoinBuildSide::Left;
    } else {
        node->join().build_side = plan::JoinBuildSide::Right;
    }
    return absl::OkStatus();
}

absl::Status ChooseJoinBuildSides(const std::shared_ptr<plan::PlanNode>& node,
                                  const HeuristicCostModel& cost_model,
                                  const ConnectorStatsProvider& stats_provider) {
    if (node == nullptr) {
        return absl::OkStatus();
    }
    for (const auto& input : node->inputs) {
        auto status = ChooseJoinBuildSides(input, cost_model, stats_provider);
        if (!status.ok()) {
            return status;
        }
    }
    return ChooseJoinBuildSide(node, cost_model, stats_provider);
}

struct ConnectorMemorySuffixPlan {
    std::string name;
    std::shared_ptr<plan::PlanNode> plan;
};

bool is_executable_connector_plan(const std::shared_ptr<plan::PlanNode>& node) {
    auto optimized_or = DefaultRuleBasedOptimizer().Optimize(node);
    return optimized_or.ok() && optimized_or->pushdown_plan.has_value() &&
           CanExecutePushdownPlan(*optimized_or->pushdown_plan);
}

void append_connector_memory_suffix_plans(const std::shared_ptr<plan::PlanNode>& node,
                                          std::vector<ConnectorMemorySuffixPlan>* plans) {
    if (node == nullptr || node->inputs.size() != 1 || node->inputs[0] == nullptr) {
        return;
    }
    const auto& input = node->inputs[0];
    if (is_executable_connector_plan(input)) {
        auto candidate = clone_plan(node);
        candidate->inputs[0] = plan::MakeMaterializeBarrier(
            clone_plan(input), "cbo connector/memory boundary", plan::PlanNodeKindName(node->kind));
        plans->push_back({
            .name = absl::StrCat(alternative_name(PhysicalShape::ConnectorPrefixMemorySuffix),
                                 "_after_",
                                 plan::PlanNodeKindName(input->kind)),
            .plan = std::move(candidate),
        });
    }

    std::vector<ConnectorMemorySuffixPlan> input_plans;
    append_connector_memory_suffix_plans(input, &input_plans);
    for (auto& input_plan : input_plans) {
        auto candidate = clone_plan(node);
        candidate->inputs[0] = std::move(input_plan.plan);
        plans->push_back({
            .name = std::move(input_plan.name),
            .plan = std::move(candidate),
        });
    }
}

double cost_score(const plan::CostEstimate& cost) {
    return cost.cpu.value_or(0.0) + cost.io.value_or(0.0);
}

size_t choose_lowest_known_cost(const std::vector<PlanAlternative>& alternatives) {
    size_t chosen = 0;
    for (size_t index = 1; index < alternatives.size(); ++index) {
        if (!alternatives[index].cost.rows.has_value()) {
            continue;
        }
        if (!alternatives[chosen].cost.rows.has_value() ||
            cost_score(alternatives[index].cost) < cost_score(alternatives[chosen].cost)) {
            chosen = index;
        }
    }
    return chosen;
}

} // namespace

CostBasedOptimizer::CostBasedOptimizer(CboOptions options) : options_(options) {}

absl::StatusOr<CboPlanResult> CostBasedOptimizer::OptimizeWithTrace(
    const std::shared_ptr<plan::PlanNode>& plan) const {
    auto rbo_or = DefaultRuleBasedOptimizer().Optimize(clone_plan(plan));
    if (!rbo_or.ok()) {
        return rbo_or.status();
    }

    ConnectorStatsProvider stats_provider(options_);
    HeuristicCostModel cost_model;
    auto choose_status = ChooseJoinBuildSides(rbo_or->plan, cost_model, stats_provider);
    if (!choose_status.ok()) {
        return choose_status;
    }
    const PhysicalShape shape = physical_shape_for(*rbo_or);
    CboPlanResult result;
    result.rbo_result = std::move(*rbo_or);
    if (shape == PhysicalShape::ConnectorScan) {
        auto connector_cost_or =
            cost_model.EstimateConnectorCost(result.rbo_result.plan, stats_provider);
        if (!connector_cost_or.ok()) {
            return connector_cost_or.status();
        }
        result.alternatives.push_back({
            .name = alternative_name(PhysicalShape::ConnectorScan),
            .shape = PhysicalShape::ConnectorScan,
            .plan = result.rbo_result.plan,
            .cost = *connector_cost_or,
        });

        std::vector<ConnectorMemorySuffixPlan> suffix_plans;
        append_connector_memory_suffix_plans(result.rbo_result.plan, &suffix_plans);
        for (auto& suffix : suffix_plans) {
            auto cost_or = cost_model.EstimateCost(suffix.plan, stats_provider);
            if (!cost_or.ok()) {
                return cost_or.status();
            }
            result.alternatives.push_back({
                .name = std::move(suffix.name),
                .shape = PhysicalShape::ConnectorPrefixMemorySuffix,
                .plan = std::move(suffix.plan),
                .cost = *cost_or,
            });
        }
    } else if (shape == PhysicalShape::LocalHashJoin) {
        const auto chosen_side = result.rbo_result.plan->join().build_side;
        const auto other_side = chosen_side == plan::JoinBuildSide::Left
                                    ? plan::JoinBuildSide::Right
                                    : plan::JoinBuildSide::Left;
        auto chosen_cost_or = EstimateJoinWithBuildSide(
            result.rbo_result.plan, chosen_side, cost_model, stats_provider);
        if (!chosen_cost_or.ok()) {
            return chosen_cost_or.status();
        }
        auto chosen_plan = clone_plan(result.rbo_result.plan);
        chosen_plan->join().build_side = chosen_side;
        result.alternatives.push_back({
            .name = join_alternative_name(chosen_side),
            .shape = PhysicalShape::LocalHashJoin,
            .plan = std::move(chosen_plan),
            .cost = *chosen_cost_or,
        });
        auto other_cost_or = EstimateJoinWithBuildSide(
            result.rbo_result.plan, other_side, cost_model, stats_provider);
        result.alternatives.push_back(PlanAlternative{
            .name = join_alternative_name(other_side),
            .shape = PhysicalShape::LocalHashJoin,
            .plan =
                [&] {
                    auto candidate_plan = clone_plan(result.rbo_result.plan);
                    candidate_plan->join().build_side = other_side;
                    return candidate_plan;
                }(),
            .cost = other_cost_or.ok() ? *other_cost_or : unknown_cost(),
        });
    } else {
        auto cost_or = cost_model.EstimateCost(result.rbo_result.plan, stats_provider);
        if (!cost_or.ok()) {
            return cost_or.status();
        }
        result.alternatives.push_back(PlanAlternative{
            .name = alternative_name(shape),
            .shape = shape,
            .plan = result.rbo_result.plan,
            .cost = *cost_or,
        });
    }

    const size_t chosen = choose_lowest_known_cost(result.alternatives);
    for (size_t index = 0; index < result.alternatives.size(); ++index) {
        auto& alternative = result.alternatives[index];
        alternative.chosen = index == chosen;
        alternative.reason =
            index == chosen
                ? (alternative.cost.rows.has_value() ? "lowest-known-cost" : "rbo-fallback")
                : "higher-estimated-cost";
    }
    result.rbo_result.plan = result.alternatives[chosen].plan;
    if (chosen != 0 && shape == PhysicalShape::ConnectorScan) {
        result.rbo_result.pushdown_plan.reset();
    }
    result.cost = result.alternatives[chosen].cost;
    result.decision = cbo_decision(options_, result.cost);
    result.has_statistics = result.cost.rows.has_value();
    return result;
}

absl::StatusOr<PlanOptimizerResult> CostBasedOptimizer::Optimize(
    std::shared_ptr<plan::PlanNode> plan) const {
    auto result_or = OptimizeWithTrace(plan);
    if (!result_or.ok()) {
        return result_or.status();
    }
    return std::move(result_or->rbo_result);
}

CostBasedOptimizer DefaultCostBasedOptimizer() {
    return CostBasedOptimizer(CboOptions{.collect_connector_stats = true});
}

CostBasedOptimizer FastCostBasedOptimizer() {
    return CostBasedOptimizer(CboOptions{.collect_connector_stats = false});
}

std::string PhysicalShapeName(PhysicalShape shape) {
    return alternative_name(shape);
}

} // namespace pl::flux::optimizer
