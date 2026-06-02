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
constexpr double kExchangeCpuPerRow = 0.2;
constexpr double kGatherContentionCpuPerRow = 0.75;
constexpr double kHashSkewCpuPerRow = 0.8;
constexpr double kSaltedSkewCpuPerRow = 0.1;
constexpr double kReplicationCpuPerRow = 0.1;
constexpr double kMinPartitionedRows = 8192.0;
constexpr double kTargetRowsPerPartition = 4096.0;
constexpr double kMaxBroadcastBuildRows = 1024.0;
constexpr double kMinBroadcastProbeRows = 8192.0;
constexpr double kMinBroadcastProbeToBuildRatio = 8.0;
constexpr double kMinHeavyHitterRows = 1024.0;
constexpr double kMinHeavyHitterFraction = 0.2;
constexpr size_t kMaxPartitions = 8;

bool has_rows(const plan::CostEstimate& cost) {
    return cost.rows.has_value();
}

struct EstimatedPlan {
    plan::CostEstimate cost;
    std::vector<connector::ColumnStatistics> columns{};
};

plan::CostEstimate add_selected_join_distribution_cost(plan::CostEstimate cost,
                                                       const EstimatedPlan& left,
                                                       const EstimatedPlan& right,
                                                       const plan::JoinSpec& join);

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

void cap_column_statistics(std::vector<connector::ColumnStatistics>* columns, double row_count) {
    for (auto& column : *columns) {
        if (column.distinct_values.has_value()) {
            column.distinct_values = std::min(std::max(0.0, *column.distinct_values), row_count);
        }
        for (auto& value : column.most_common_values) {
            value.frequency = std::min(std::max(0.0, value.frequency), row_count);
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
    cap_column_statistics(&columns, output_rows);
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

void append_missing_columns(std::vector<connector::ColumnStatistics>* columns,
                            const std::vector<connector::ColumnStatistics>& candidates) {
    for (const auto& column : candidates) {
        if (find_column(*columns, column.name) == nullptr) {
            columns->push_back(column);
        }
    }
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
    cap_column_statistics(&columns, output_rows);
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

    [[nodiscard]] absl::StatusOr<EstimatedPlan> EstimatePlan(
        const std::shared_ptr<plan::PlanNode>& node, const StatsProvider& stats_provider) const {
        return estimate(node, stats_provider, ExecutionLocation::Memory);
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
            auto cost = plan::CostEstimate{
                .rows = output_rows,
                .cpu = left_or->cost.cpu.value_or(0.0) + right_or->cost.cpu.value_or(0.0) +
                       build_rows * kJoinBuildCpuPerRow + probe_rows * kJoinProbeCpuPerRow,
                .io = left_or->cost.io.value_or(0.0) + right_or->cost.io.value_or(0.0),
            };
            cost = add_selected_join_distribution_cost(cost, *left_or, *right_or, node->join());
            auto columns = std::move(left_or->columns);
            append_missing_columns(&columns, right_or->columns);
            cap_column_statistics(&columns, output_rows);
            return EstimatedPlan{
                .cost = cost,
                .columns = std::move(columns),
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
                extra_cpu = input_rows * kExchangeCpuPerRow;
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

std::string join_distribution_alternative_name(const plan::JoinSpec& join) {
    return absl::StrCat(join_alternative_name(join.build_side),
                        "_",
                        plan::JoinDistributionKindName(join.distribution.kind));
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

size_t choose_lowest_known_cost(const std::vector<PlanAlternative>& alternatives);

size_t join_partition_count(double input_rows) {
    if (input_rows < kMinPartitionedRows) {
        return 1;
    }
    return std::min(
        kMaxPartitions,
        std::max<size_t>(2, static_cast<size_t>(std::ceil(input_rows / kTargetRowsPerPartition))));
}

const connector::MostCommonValue* find_most_common_value(const connector::ColumnStatistics* column,
                                                         const std::string& value) {
    if (column == nullptr) {
        return nullptr;
    }
    const auto it = std::ranges::find_if(column->most_common_values,
                                         [&](const auto& item) { return item.value == value; });
    return it == column->most_common_values.end() ? nullptr : &*it;
}

struct JoinSkewEstimate {
    std::vector<std::string> heavy_hitters{};
    double probe_rows = 0.0;
    double replicated_build_rows = 0.0;
};

JoinSkewEstimate estimate_join_skew(const EstimatedPlan& build,
                                    const EstimatedPlan& probe,
                                    const plan::JoinSpec& join) {
    JoinSkewEstimate estimate;
    if (join.on.size() != 1 || !probe.cost.rows.has_value()) {
        return estimate;
    }
    const auto& key = join.on.front();
    const auto* probe_column = find_column(probe.columns, key);
    const auto* build_column = find_column(build.columns, key);
    if (probe_column == nullptr) {
        return estimate;
    }
    for (const auto& value : probe_column->most_common_values) {
        if (value.frequency < kMinHeavyHitterRows ||
            value.frequency < probe.cost.rows.value_or(0.0) * kMinHeavyHitterFraction) {
            continue;
        }
        estimate.heavy_hitters.push_back(absl::StrCat(key, "=", value.value, "\n"));
        estimate.probe_rows += value.frequency;
        if (const auto* build_value = find_most_common_value(build_column, value.value);
            build_value != nullptr) {
            estimate.replicated_build_rows += build_value->frequency;
        } else if (build_column != nullptr && build.cost.rows.has_value() &&
                   build_column->distinct_values.has_value() &&
                   *build_column->distinct_values > 0.0) {
            estimate.replicated_build_rows +=
                build.cost.rows.value_or(0.0) / *build_column->distinct_values;
        }
    }
    return estimate;
}

struct JoinDistributionCostInput {
    size_t partitions = 1;
    double left_rows = 0.0;
    double right_rows = 0.0;
    double build_rows = 0.0;
};

plan::CostEstimate add_join_distribution_cost(plan::CostEstimate cost,
                                              plan::JoinDistributionKind kind,
                                              const JoinDistributionCostInput& input,
                                              const JoinSkewEstimate& skew) {
    if (!cost.cpu.has_value()) {
        return cost;
    }
    const double input_rows = input.left_rows + input.right_rows;
    switch (kind) {
        case plan::JoinDistributionKind::Gather:
            *cost.cpu +=
                input_rows >= kMinPartitionedRows ? input_rows * kGatherContentionCpuPerRow : 0.0;
            break;
        case plan::JoinDistributionKind::Hash:
            *cost.cpu += input_rows * kExchangeCpuPerRow + skew.probe_rows * kHashSkewCpuPerRow;
            break;
        case plan::JoinDistributionKind::Broadcast:
            *cost.cpu += input_rows * kExchangeCpuPerRow +
                         input.build_rows * static_cast<double>(input.partitions - 1) *
                             kReplicationCpuPerRow;
            break;
        case plan::JoinDistributionKind::Salted:
            *cost.cpu += input_rows * kExchangeCpuPerRow + skew.probe_rows * kSaltedSkewCpuPerRow +
                         skew.replicated_build_rows * static_cast<double>(input.partitions - 1) *
                             kReplicationCpuPerRow;
            break;
        case plan::JoinDistributionKind::Auto:
            break;
    }
    return cost;
}

plan::CostEstimate add_selected_join_distribution_cost(plan::CostEstimate cost,
                                                       const EstimatedPlan& left,
                                                       const EstimatedPlan& right,
                                                       const plan::JoinSpec& join) {
    if (!left.cost.rows.has_value() || !right.cost.rows.has_value()) {
        return cost;
    }
    const bool build_left = join.build_side == plan::JoinBuildSide::Left;
    const auto& build = build_left ? left : right;
    const auto& probe = build_left ? right : left;
    return add_join_distribution_cost(cost,
                                      join.distribution.kind,
                                      {
                                          .partitions = join.distribution.partitions,
                                          .left_rows = left.cost.rows.value_or(0.0),
                                          .right_rows = right.cost.rows.value_or(0.0),
                                          .build_rows = build.cost.rows.value_or(0.0),
                                      },
                                      estimate_join_skew(build, probe, join));
}

absl::StatusOr<std::vector<PlanAlternative>> EnumerateJoinAlternatives(
    const std::shared_ptr<plan::PlanNode>& node,
    const HeuristicCostModel& cost_model,
    const ConnectorStatsProvider& stats_provider) {
    std::vector<PlanAlternative> alternatives;
    if (node == nullptr || node->kind != plan::PlanNodeKind::Join || node->inputs.size() != 2 ||
        node->inputs[0] == nullptr || node->inputs[1] == nullptr) {
        return alternatives;
    }
    auto left_or = cost_model.EstimatePlan(node->inputs[0], stats_provider);
    auto right_or = cost_model.EstimatePlan(node->inputs[1], stats_provider);
    if (!left_or.ok() || !right_or.ok() || !left_or->cost.rows.has_value() ||
        !right_or->cost.rows.has_value()) {
        return alternatives;
    }
    const double left_rows = *left_or->cost.rows;
    const double right_rows = *right_or->cost.rows;
    const size_t partitions = join_partition_count(left_rows + right_rows);
    for (const auto side : {plan::JoinBuildSide::Left, plan::JoinBuildSide::Right}) {
        const bool build_left = side == plan::JoinBuildSide::Left;
        const auto& build = build_left ? *left_or : *right_or;
        const auto& probe = build_left ? *right_or : *left_or;
        const double build_rows = build_left ? left_rows : right_rows;
        const double probe_rows = build_left ? right_rows : left_rows;
        const auto skew = estimate_join_skew(build, probe, node->join());
        auto append = [&](plan::JoinDistributionKind kind,
                          std::vector<std::string> heavy_hitters = {}) {
            auto candidate = clone_plan(node);
            candidate->join().build_side = side;
            candidate->join().distribution = {
                .kind = kind,
                .partitions = kind == plan::JoinDistributionKind::Gather ? 1 : partitions,
                .heavy_hitters = std::move(heavy_hitters),
            };
            auto cost_or = EstimateJoinWithBuildSide(candidate, side, cost_model, stats_provider);
            alternatives.push_back({
                .name = join_distribution_alternative_name(candidate->join()),
                .shape = PhysicalShape::LocalHashJoin,
                .plan = std::move(candidate),
                .cost = cost_or.ok() ? *cost_or : unknown_cost(),
            });
        };
        append(plan::JoinDistributionKind::Gather);
        if (partitions <= 1) {
            continue;
        }
        append(plan::JoinDistributionKind::Hash);
        if (node->join().method == plan::JoinMethod::Inner && build_rows > 0.0 &&
            build_rows <= kMaxBroadcastBuildRows && probe_rows >= kMinBroadcastProbeRows &&
            probe_rows >= build_rows * kMinBroadcastProbeToBuildRatio) {
            append(plan::JoinDistributionKind::Broadcast);
        }
        if (node->join().method == plan::JoinMethod::Inner && build_rows > kMaxBroadcastBuildRows &&
            !skew.heavy_hitters.empty() && skew.replicated_build_rows > 0.0) {
            append(plan::JoinDistributionKind::Salted, skew.heavy_hitters);
        }
    }
    return alternatives;
}

absl::Status ChooseJoinDistributions(const std::shared_ptr<plan::PlanNode>& node,
                                     const HeuristicCostModel& cost_model,
                                     const ConnectorStatsProvider& stats_provider) {
    if (node == nullptr) {
        return absl::OkStatus();
    }
    for (const auto& input : node->inputs) {
        auto status = ChooseJoinDistributions(input, cost_model, stats_provider);
        if (!status.ok()) {
            return status;
        }
    }
    if (node->kind != plan::PlanNodeKind::Join) {
        return absl::OkStatus();
    }
    auto alternatives_or = EnumerateJoinAlternatives(node, cost_model, stats_provider);
    if (!alternatives_or.ok()) {
        return alternatives_or.status();
    }
    if (!alternatives_or->empty()) {
        const size_t chosen = choose_lowest_known_cost(*alternatives_or);
        node->join() = (*alternatives_or)[chosen].plan->join();
    }
    return absl::OkStatus();
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
    choose_status = ChooseJoinDistributions(rbo_or->plan, cost_model, stats_provider);
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
        auto alternatives_or =
            EnumerateJoinAlternatives(result.rbo_result.plan, cost_model, stats_provider);
        if (!alternatives_or.ok()) {
            return alternatives_or.status();
        }
        result.alternatives = std::move(*alternatives_or);
        if (result.alternatives.empty()) {
            auto cost_or = cost_model.EstimateCost(result.rbo_result.plan, stats_provider);
            if (!cost_or.ok()) {
                return cost_or.status();
            }
            result.alternatives.push_back({
                .name = join_distribution_alternative_name(result.rbo_result.plan->join()),
                .shape = PhysicalShape::LocalHashJoin,
                .plan = result.rbo_result.plan,
                .cost = *cost_or,
            });
        }
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
