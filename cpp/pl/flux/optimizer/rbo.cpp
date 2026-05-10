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
#include <algorithm>
#include <optional>
#include <utility>

namespace pl::flux::optimizer {
namespace {

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

class TraceOnlyPushdownRule final : public Rule {
public:
    TraceOnlyPushdownRule(std::string name, std::vector<plan::PlanNodeKind> kinds)
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
        };
    }
};

std::unique_ptr<Rule> MakeTraceOnlyPushdownRule(std::string name,
                                                std::vector<plan::PlanNodeKind> kinds) {
    return std::unique_ptr<Rule>(new TraceOnlyPushdownRule(std::move(name), std::move(kinds)));
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
    return result;
}

RuleBasedOptimizer DefaultRuleBasedOptimizer() {
    std::vector<std::unique_ptr<Rule>> rules;
    rules.push_back(
        MakeTraceOnlyPushdownRule("PushLimitIntoConnectorScan", {plan::PlanNodeKind::Limit}));
    rules.push_back(
        MakeTraceOnlyPushdownRule("PushSortIntoConnectorScan", {plan::PlanNodeKind::Sort}));
    rules.push_back(
        MakeTraceOnlyPushdownRule("PushProjectionIntoConnectorScan",
                                  {plan::PlanNodeKind::Project, plan::PlanNodeKind::Rename}));
    rules.push_back(
        MakeTraceOnlyPushdownRule("PushPredicateIntoConnectorScan", {plan::PlanNodeKind::Filter}));
    rules.push_back(
        MakeTraceOnlyPushdownRule("PushTimeRangeIntoConnectorScan", {plan::PlanNodeKind::Range}));
    rules.push_back(
        MakeTraceOnlyPushdownRule("PushDistinctIntoConnectorScan", {plan::PlanNodeKind::Distinct}));
    rules.push_back(
        MakeTraceOnlyPushdownRule("PushAggregateIntoConnectorScan",
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

bool IsPushdownSourceScan(const plan::PlanNode& node) {
    if (node.kind != plan::PlanNodeKind::SourceScan) {
        return false;
    }
    return (node.source_scan.source == "sqlite" && node.source_scan.driver == "sqlite") ||
           (node.source_scan.source == "mysql" && node.source_scan.driver == "mysql");
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
            return true;
        case plan::PlanNodeKind::Filter:
            return !node.filter.predicates.empty();
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
        return node.source_scan.source;
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
