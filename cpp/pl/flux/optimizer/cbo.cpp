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

#include "absl/status/status.h"
#include "cpp/pl/flux/connector/connector_registry.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace pl::flux::optimizer {
namespace {

constexpr double kRangeSelectivity = 0.25;
constexpr double kFilterSelectivity = 0.5;
constexpr double kProjectCpuPerRow = 0.05;
constexpr double kMemoryCpuPerRow = 1.0;
constexpr double kConnectorCpuPerRow = 0.05;

bool has_rows(const plan::CostEstimate& cost) { return cost.rows.has_value(); }

plan::CostEstimate unknown_cost() { return {}; }

double safe_non_negative(int64_t value) { return static_cast<double>(std::max<int64_t>(0, value)); }

plan::CostEstimate combine_unary_cost(const plan::CostEstimate& input,
                                      double output_rows,
                                      double extra_cpu,
                                      double extra_io) {
    plan::CostEstimate cost;
    cost.rows = output_rows;
    cost.cpu = input.cpu.value_or(0.0) + extra_cpu;
    cost.io = input.io.value_or(0.0) + extra_io;
    return cost;
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
        connector::SourceSpec spec{source.source, source.driver, source.dsn, source.table};
        auto source_or = connector::ConnectorRegistry::Global().Create(spec);
        if (!source_or.ok()) {
            return PlanStatistics{};
        }
        auto statistics_or = (*source_or)->Statistics();
        if (!statistics_or.ok()) {
            return PlanStatistics{};
        }
        return PlanStatistics{
            .row_count = statistics_or->row_count,
            .size_bytes = statistics_or->size_bytes,
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
        return estimate(node, stats_provider);
    }

private:
    [[nodiscard]] absl::StatusOr<plan::CostEstimate> estimate(
        const std::shared_ptr<plan::PlanNode>& node, const StatsProvider& stats_provider) const {
        if (node->kind == plan::PlanNodeKind::SourceScan) {
            auto stats_or = stats_provider.Estimate(node);
            if (!stats_or.ok() || !stats_or->row_count.has_value()) {
                return unknown_cost();
            }
            const double rows = *stats_or->row_count;
            return plan::CostEstimate{
                .rows = rows,
                .cpu = rows * kConnectorCpuPerRow,
                .io = stats_or->size_bytes.value_or(rows),
            };
        }

        if (node->inputs.size() != 1 || node->inputs[0] == nullptr) {
            return unknown_cost();
        }
        auto input_or = estimate(node->inputs[0], stats_provider);
        if (!input_or.ok()) {
            return input_or.status();
        }
        if (!has_rows(*input_or)) {
            return unknown_cost();
        }

        const double input_rows = *input_or->rows;
        double output_rows = input_rows;
        double extra_cpu = input_rows * kMemoryCpuPerRow;
        switch (node->kind) {
            case plan::PlanNodeKind::Range:
                output_rows = input_rows * kRangeSelectivity;
                break;
            case plan::PlanNodeKind::Filter:
                output_rows =
                    input_rows * std::pow(kFilterSelectivity,
                                          static_cast<double>(node->filter().predicates.size()));
                break;
            case plan::PlanNodeKind::Project:
            case plan::PlanNodeKind::Rename:
                extra_cpu = input_rows * kProjectCpuPerRow;
                break;
            case plan::PlanNodeKind::Limit:
                output_rows = std::min(input_rows, safe_non_negative(node->limit().n));
                break;
            case plan::PlanNodeKind::Sort:
                extra_cpu = input_rows * std::log2(std::max(2.0, input_rows));
                break;
            case plan::PlanNodeKind::Group:
                output_rows = std::min(input_rows, std::max(1.0, std::sqrt(input_rows)));
                break;
            case plan::PlanNodeKind::Aggregate:
                output_rows = node->inputs[0]->kind == plan::PlanNodeKind::Group ? input_rows : 1.0;
                break;
            case plan::PlanNodeKind::Distinct:
                output_rows = std::min(input_rows, std::max(1.0, std::sqrt(input_rows)));
                break;
            case plan::PlanNodeKind::Materialize:
                extra_cpu = input_rows * 0.1;
                break;
            default:
                return unknown_cost();
        }
        return combine_unary_cost(*input_or, output_rows, extra_cpu, 0.0);
    }
};

PhysicalShape physical_shape_for(const PlanOptimizerResult& rbo_result) {
    if (rbo_result.pushdown_plan.has_value() && CanExecutePushdownPlan(*rbo_result.pushdown_plan)) {
        return PhysicalShape::ConnectorScan;
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

} // namespace

CostBasedOptimizer::CostBasedOptimizer(CboOptions options) : options_(options) {}

absl::StatusOr<CboPlanResult> CostBasedOptimizer::OptimizeWithTrace(
    std::shared_ptr<plan::PlanNode> plan) const {
    auto rbo_or = DefaultRuleBasedOptimizer().Optimize(std::move(plan));
    if (!rbo_or.ok()) {
        return rbo_or.status();
    }

    ConnectorStatsProvider stats_provider(options_);
    HeuristicCostModel cost_model;
    auto cost_or = cost_model.EstimateCost(rbo_or->plan, stats_provider);
    if (!cost_or.ok()) {
        return cost_or.status();
    }

    const PhysicalShape shape = physical_shape_for(*rbo_or);
    PlanAlternative chosen{
        .name = alternative_name(shape),
        .shape = shape,
        .cost = *cost_or,
        .chosen = true,
        .reason = cost_or->rows.has_value() ? "lowest-known-cost" : "rbo-fallback",
    };

    CboPlanResult result;
    result.rbo_result = std::move(*rbo_or);
    result.decision = cbo_decision(options_, *cost_or);
    result.cost = *cost_or;
    result.alternatives.push_back(std::move(chosen));
    result.has_statistics = result.cost.rows.has_value();
    return result;
}

absl::StatusOr<PlanOptimizerResult> CostBasedOptimizer::Optimize(
    std::shared_ptr<plan::PlanNode> plan) const {
    auto result_or = OptimizeWithTrace(std::move(plan));
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

std::string PhysicalShapeName(PhysicalShape shape) { return alternative_name(shape); }

} // namespace pl::flux::optimizer
