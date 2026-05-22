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

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/flux/connector/table_source.h"
#include "cpp/pl/flux/optimizer/rbo.h"
#include "cpp/pl/flux/plan/physical_plan.h"

namespace pl::flux::optimizer {

struct CboOptions {
    bool collect_connector_stats = true;
};

struct PlanStatistics {
    std::optional<double> row_count;
    std::optional<double> size_bytes;
    std::vector<connector::ColumnStatistics> columns;
};

enum class PhysicalShape {
    ConnectorScan,
    ConnectorPrefixMemorySuffix,
    LocalHashJoin,
    Exchange,
    MemoryScan,
};

struct PlanAlternative {
    std::string name;
    PhysicalShape shape = PhysicalShape::MemoryScan;
    plan::CostEstimate cost;
    bool chosen = false;
    std::string reason;
};

struct CboPlanResult {
    PlanOptimizerResult rbo_result;
    std::string decision;
    plan::CostEstimate cost;
    std::vector<PlanAlternative> alternatives;
    bool has_statistics = false;
};

class StatsProvider {
public:
    virtual ~StatsProvider() = default;

    [[nodiscard]] virtual absl::StatusOr<PlanStatistics> Estimate(
        const std::shared_ptr<plan::PlanNode>& plan) const = 0;
};

class CostModel {
public:
    virtual ~CostModel() = default;

    [[nodiscard]] virtual absl::StatusOr<plan::CostEstimate> EstimateCost(
        const std::shared_ptr<plan::PlanNode>& plan, const StatsProvider& stats_provider) const = 0;
};

class CostBasedOptimizer final : public PlanOptimizer {
public:
    explicit CostBasedOptimizer(CboOptions options);

    [[nodiscard]] absl::StatusOr<CboPlanResult> OptimizeWithTrace(
        std::shared_ptr<plan::PlanNode> plan) const;

    [[nodiscard]] absl::StatusOr<PlanOptimizerResult> Optimize(
        std::shared_ptr<plan::PlanNode> plan) const override;

private:
    CboOptions options_;
};

CostBasedOptimizer DefaultCostBasedOptimizer();
CostBasedOptimizer FastCostBasedOptimizer();
std::string PhysicalShapeName(PhysicalShape shape);

} // namespace pl::flux::optimizer
