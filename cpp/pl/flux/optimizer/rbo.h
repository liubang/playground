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
#include "cpp/pl/flux/plan/plan_node.h"

namespace pl::flux::optimizer {

enum class PushdownState {
    SourceScan,
    SourcePushdown,
    MaterializeBarrier,
    Memory,
};

struct PushdownPlan {
    const plan::SourceScanSpec* source = nullptr;
    connector::ScanRequest request;
    connector::SourceCapabilities capabilities;
    std::vector<std::string> visible_columns;
    std::vector<std::string> source_columns;
};

struct RuleTrace {
    std::string rule;
    bool applied = false;
    std::string detail;
};

struct RuleApplication {
    std::shared_ptr<plan::PlanNode> plan;
    bool applied = false;
    std::string detail;
};

struct PlanOptimizerResult {
    std::shared_ptr<plan::PlanNode> plan;
    std::vector<RuleTrace> trace;
    std::optional<PushdownPlan> pushdown_plan;
};

class Rule {
public:
    virtual ~Rule() = default;

    [[nodiscard]] virtual std::string Name() const = 0;
    [[nodiscard]] virtual absl::StatusOr<RuleApplication> Apply(
        const std::shared_ptr<plan::PlanNode>& plan) const = 0;
};

class PlanOptimizer {
public:
    virtual ~PlanOptimizer() = default;

    [[nodiscard]] virtual absl::StatusOr<PlanOptimizerResult> Optimize(
        std::shared_ptr<plan::PlanNode> plan) const = 0;
};

class RuleBasedOptimizer final : public PlanOptimizer {
public:
    explicit RuleBasedOptimizer(std::vector<std::unique_ptr<Rule>> rules);

    [[nodiscard]] absl::StatusOr<PlanOptimizerResult> Optimize(
        std::shared_ptr<plan::PlanNode> plan) const override;

private:
    std::vector<std::unique_ptr<Rule>> rules_;
};

RuleBasedOptimizer DefaultRuleBasedOptimizer();

std::vector<std::string> AppliedRuleNames(const PlanOptimizerResult& result);

absl::StatusOr<std::vector<std::string>> SourceScanColumns(const plan::SourceScanSpec& source);
absl::StatusOr<connector::SourceCapabilities> SourceScanCapabilities(
    const plan::SourceScanSpec& source);
absl::StatusOr<std::vector<std::string>> VisibleColumnsForPlan(
    const std::shared_ptr<plan::PlanNode>& node);
bool CanExecutePushdownPlan(const PushdownPlan& plan);
std::string FormatSourceCapabilities(const connector::SourceCapabilities& capabilities);
std::string FormatPushdownRequest(const connector::ScanRequest& request);
std::optional<std::string> SourcePushdownSummary(const PlanOptimizerResult& result);
bool IsPushdownSourceScan(const plan::PlanNode& node);
bool IsPushableUnaryNode(const plan::PlanNode& node);
PushdownState AnalyzePushdownState(const plan::PlanNode& node);
std::optional<std::string> PushdownSourceName(const plan::PlanNode& node);
bool ContainsPlanNodeKind(const plan::PlanNode& node, plan::PlanNodeKind kind);
bool IsExecutableConnectorPrefix(const plan::PlanNode& node);

} // namespace pl::flux::optimizer
