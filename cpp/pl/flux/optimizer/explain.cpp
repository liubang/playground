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

#include "cpp/pl/flux/optimizer/explain.h"

#include "cpp/pl/flux/optimizer/cbo.h"
#include "cpp/pl/flux/optimizer/rbo.h"
#include "cpp/pl/flux/plan/physical_plan.h"
#include <sstream>
#include <vector>

namespace pl::flux::optimizer {
namespace {

void ApplyCboTrace(const CboPlanResult& cbo_result, plan::PhysicalPlanNode* physical) {
    physical->optimizer.rbo_rules = AppliedRuleNames(cbo_result.rbo_result);
    physical->optimizer.cbo_decision = cbo_result.decision;
    physical->optimizer.cost = cbo_result.cost;
}

void ApplyCboTraceForPlan(const std::shared_ptr<plan::PlanNode>& node,
                          plan::PhysicalPlanNode* physical) {
    auto cbo_or = DefaultCostBasedOptimizer().OptimizeWithTrace(node);
    if (cbo_or.ok()) {
        ApplyCboTrace(*cbo_or, physical);
    }
}

void FormatPushdownState(const plan::PlanNode& node, std::ostringstream* out) {
    switch (AnalyzePushdownState(node)) {
        case PushdownState::SourceScan: {
            auto source = PushdownSourceName(node).value_or("source");
            *out << " [" << source << " scan]";
            return;
        }
        case PushdownState::SourcePushdown: {
            auto source = PushdownSourceName(node).value_or("source");
            *out << " [" << source << " pushdown";
            if (node.kind == plan::PlanNodeKind::Filter && !node.filter().predicates.empty()) {
                *out << ": " << plan::PredicateListString(node.filter().predicates);
            }
            *out << "]";
            return;
        }
        case PushdownState::MaterializeBarrier:
            *out << " [barrier";
            if (!node.materialize().reason.empty()) {
                *out << ": " << node.materialize().reason;
            }
            *out << "]";
            return;
        case PushdownState::Memory:
            *out << " [memory]";
            return;
    }
}

void FormatLogicalNodeDetail(const plan::PlanNode& node, std::ostringstream* out) {
    *out << plan::PlanNodeKindName(node.kind);
    FormatPushdownState(node, out);
    if (node.kind == plan::PlanNodeKind::SourceScan) {
        *out << "(source=\"" << node.source_scan().source << "\", driver=\""
             << node.source_scan().driver << "\", table=\"" << node.source_scan().table << "\")";
    } else if (node.kind == plan::PlanNodeKind::Materialize &&
               (!node.materialize().reason.empty() || !node.materialize().builtin.empty())) {
        *out << "(";
        bool needs_separator = false;
        if (!node.materialize().reason.empty()) {
            *out << "reason=\"" << node.materialize().reason << "\"";
            needs_separator = true;
        }
        if (!node.materialize().builtin.empty()) {
            if (needs_separator) {
                *out << ", ";
            }
            *out << "builtin=\"" << node.materialize().builtin << "\"";
        }
        *out << ")";
    }
}

void FormatLogicalPlanTree(const plan::PlanNode& node,
                           const std::string& prefix,
                           bool is_last,
                           bool is_root,
                           std::ostringstream* out) {
    if (is_root) {
        FormatLogicalNodeDetail(node, out);
    } else {
        *out << prefix << (is_last ? "`- " : "|- ");
        FormatLogicalNodeDetail(node, out);
    }
    *out << "\n";

    const std::string child_prefix = is_root ? "" : prefix + (is_last ? "   " : "|  ");
    for (size_t i = 0; i < node.inputs.size(); ++i) {
        if (node.inputs[i] != nullptr) {
            FormatLogicalPlanTree(*node.inputs[i], child_prefix, i + 1 == node.inputs.size(), false,
                                  out);
        }
    }
}

std::string FormatLogicalPlanOnly(const std::shared_ptr<plan::PlanNode>& node) {
    if (node == nullptr) {
        return "<no plan>";
    }
    std::ostringstream out;
    FormatLogicalPlanTree(*node, "", true, true, &out);
    return out.str();
}

void CollectLogicalPrefix(const plan::PlanNode& node, std::vector<std::string>* nodes) {
    nodes->push_back(plan::PlanNodeKindName(node.kind));
    if (node.inputs.size() == 1 && node.inputs[0] != nullptr &&
        (IsPushableUnaryNode(node) || node.kind == plan::PlanNodeKind::SourceScan)) {
        CollectLogicalPrefix(*node.inputs[0], nodes);
    }
}

std::shared_ptr<plan::PhysicalPlanNode> MakeConnectorPhysicalPlan(
    const std::shared_ptr<plan::PlanNode>& node) {
    auto physical = std::make_shared<plan::PhysicalPlanNode>();
    physical->kind = plan::PhysicalNodeKind::ConnectorScan;
    physical->name = "connector scan";
    physical->source = PushdownSourceName(*node).value_or("source");
    physical->driver = physical->source;
    physical->lazy = true;
    CollectLogicalPrefix(*node, &physical->logical_prefix);
    ApplyCboTraceForPlan(node, physical.get());
    return physical;
}

std::shared_ptr<plan::PhysicalPlanNode> BuildPhysicalPlan(
    const std::shared_ptr<plan::PlanNode>& node) {
    if (node == nullptr) {
        return nullptr;
    }

    const PushdownState pushdown_state = AnalyzePushdownState(*node);
    if (pushdown_state == PushdownState::SourceScan ||
        (pushdown_state == PushdownState::SourcePushdown && IsExecutableConnectorPrefix(*node))) {
        return MakeConnectorPhysicalPlan(node);
    }

    auto physical = std::make_shared<plan::PhysicalPlanNode>();
    physical->kind = node->kind == plan::PlanNodeKind::Materialize
                         ? plan::PhysicalNodeKind::Materialize
                         : plan::PhysicalNodeKind::MemoryOperator;
    physical->name = plan::PlanNodeKindName(node->kind);
    physical->lazy = false;
    ApplyCboTraceForPlan(node, physical.get());
    if (node->kind != plan::PlanNodeKind::Materialize) {
        physical->optimizer.rbo_rules.clear();
    }
    for (const auto& input : node->inputs) {
        physical->inputs.push_back(BuildPhysicalPlan(input));
    }
    if (physical->inputs.empty() && node->kind == plan::PlanNodeKind::SourceScan) {
        physical->kind = plan::PhysicalNodeKind::MemoryScan;
        physical->source = node->source_scan().source;
        physical->driver = node->source_scan().driver;
    }
    return physical;
}

} // namespace

std::string FormatLogicalPlan(const std::shared_ptr<plan::PlanNode>& plan) {
    std::string out = FormatLogicalPlanOnly(plan);
    auto optimized_or = DefaultRuleBasedOptimizer().Optimize(plan);
    if (optimized_or.ok()) {
        auto summary = SourcePushdownSummary(*optimized_or);
        if (!summary.has_value()) {
            return out;
        }
        out += *summary;
        out += "\n";
    }
    return out;
}

std::string FormatOptimizedLogicalPlan(const std::shared_ptr<plan::PlanNode>& plan) {
    if (plan == nullptr) {
        return "OptimizedLogicalPlan\n<no plan>\n";
    }
    auto optimized_or = DefaultRuleBasedOptimizer().Optimize(plan);
    if (!optimized_or.ok()) {
        return optimized_or.status().ToString() + "\n";
    }
    std::ostringstream out;
    out << "OptimizedLogicalPlan\n";
    out << FormatLogicalPlanOnly(optimized_or->plan);
    const auto rules = AppliedRuleNames(*optimized_or);
    if (!rules.empty()) {
        out << "RBO(rules=" << plan::StringList(rules) << ")\n";
    }
    if (auto summary = SourcePushdownSummary(*optimized_or); summary.has_value()) {
        out << *summary << "\n";
    }
    return out.str();
}

std::string FormatPhysicalPlan(const std::shared_ptr<plan::PlanNode>& plan) {
    if (plan == nullptr) {
        return "<no physical plan>\n";
    }
    auto cbo_or = DefaultCostBasedOptimizer().OptimizeWithTrace(plan);
    if (!cbo_or.ok()) {
        return cbo_or.status().ToString() + "\n";
    }
    return plan::FormatPhysicalPlan(BuildPhysicalPlan(cbo_or->rbo_result.plan));
}

} // namespace pl::flux::optimizer
