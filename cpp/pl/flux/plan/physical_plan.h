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

#include "cpp/pl/flux/plan/plan_node.h"
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace pl::flux::plan {

enum class PhysicalNodeKind {
    ConnectorScan,
    MemoryScan,
    MemoryOperator,
    Materialize,
};

struct CostEstimate {
    std::optional<double> rows;
    std::optional<double> cpu;
    std::optional<double> io;
};

struct OptimizerTrace {
    std::vector<std::string> rbo_rules;
    std::string cbo_decision = "not-run";
    CostEstimate cost;
};

struct PhysicalPlanNode {
    PhysicalNodeKind kind = PhysicalNodeKind::MemoryOperator;
    std::string name;
    std::string source;
    std::string driver;
    std::vector<std::string> logical_prefix;
    bool lazy = false;
    OptimizerTrace optimizer;
    std::vector<std::shared_ptr<PhysicalPlanNode>> inputs;
};

inline std::string PhysicalNodeKindName(PhysicalNodeKind kind) {
    switch (kind) {
        case PhysicalNodeKind::ConnectorScan:
            return "ConnectorScan";
        case PhysicalNodeKind::MemoryScan:
            return "MemoryScan";
        case PhysicalNodeKind::MemoryOperator:
            return "MemoryOperator";
        case PhysicalNodeKind::Materialize:
            return "Materialize";
    }
    return "MemoryOperator";
}

inline void CollectLogicalPrefix(const PlanNode& node, std::vector<std::string>* nodes) {
    nodes->push_back(PlanNodeKindName(node.kind));
    if (node.inputs.size() == 1 && node.inputs[0] != nullptr &&
        (IsPushableUnaryNode(node) || node.kind == PlanNodeKind::SourceScan)) {
        CollectLogicalPrefix(*node.inputs[0], nodes);
    }
}

inline void AddRuleForNode(const PlanNode& node, std::vector<std::string>* rules) {
    switch (node.kind) {
        case PlanNodeKind::Range:
            rules->push_back("PushTimeRangeIntoConnectorScan");
            return;
        case PlanNodeKind::Filter:
            rules->push_back("PushPredicateIntoConnectorScan");
            return;
        case PlanNodeKind::Project:
        case PlanNodeKind::Rename:
            rules->push_back("PushProjectionIntoConnectorScan");
            return;
        case PlanNodeKind::Limit:
            rules->push_back("PushLimitIntoConnectorScan");
            return;
        case PlanNodeKind::Sort:
            rules->push_back("PushSortIntoConnectorScan");
            return;
        case PlanNodeKind::Distinct:
            rules->push_back("PushDistinctIntoConnectorScan");
            return;
        case PlanNodeKind::Group:
        case PlanNodeKind::Aggregate:
            rules->push_back("PushAggregateIntoConnectorScan");
            return;
        default:
            return;
    }
}

inline void CollectRulesForPrefix(const PlanNode& node, std::vector<std::string>* rules) {
    AddRuleForNode(node, rules);
    if (node.inputs.size() == 1 && node.inputs[0] != nullptr && IsPushableUnaryNode(node)) {
        CollectRulesForPrefix(*node.inputs[0], rules);
    }
}

inline std::shared_ptr<PhysicalPlanNode> MakeConnectorPhysicalPlan(const PlanNode& node) {
    auto physical = std::make_shared<PhysicalPlanNode>();
    physical->kind = PhysicalNodeKind::ConnectorScan;
    physical->name = "connector scan";
    physical->source = PushdownSourceName(node).value_or("source");
    physical->driver = physical->source;
    physical->lazy = true;
    CollectLogicalPrefix(node, &physical->logical_prefix);
    CollectRulesForPrefix(node, &physical->optimizer.rbo_rules);
    return physical;
}

inline bool ContainsPlanNodeKind(const PlanNode& node, PlanNodeKind kind) {
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

inline bool IsExecutableConnectorPrefix(const PlanNode& node) {
    if (!ContainsPlanNodeKind(node, PlanNodeKind::Group)) {
        return true;
    }
    return ContainsPlanNodeKind(node, PlanNodeKind::Aggregate);
}

inline std::shared_ptr<PhysicalPlanNode> BuildPhysicalPlan(const std::shared_ptr<PlanNode>& plan) {
    if (plan == nullptr) {
        return nullptr;
    }

    const PushdownState pushdown_state = AnalyzePushdownState(*plan);
    if (pushdown_state == PushdownState::SourceScan ||
        (pushdown_state == PushdownState::SourcePushdown && IsExecutableConnectorPrefix(*plan))) {
        return MakeConnectorPhysicalPlan(*plan);
    }

    auto physical = std::make_shared<PhysicalPlanNode>();
    physical->kind = plan->kind == PlanNodeKind::Materialize ? PhysicalNodeKind::Materialize
                                                             : PhysicalNodeKind::MemoryOperator;
    physical->name = PlanNodeKindName(plan->kind);
    physical->lazy = false;
    if (plan->kind == PlanNodeKind::Materialize) {
        physical->optimizer.rbo_rules.push_back("InsertMaterializationBarrier");
    }
    for (const auto& input : plan->inputs) {
        physical->inputs.push_back(BuildPhysicalPlan(input));
    }
    if (physical->inputs.empty() && plan->kind == PlanNodeKind::SourceScan) {
        physical->kind = PhysicalNodeKind::MemoryScan;
        physical->source = plan->source_scan.source;
        physical->driver = plan->source_scan.driver;
    }
    return physical;
}

inline std::string StringList(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << values[i];
    }
    out << "]";
    return out.str();
}

inline std::string CostString(const CostEstimate& cost) {
    if (!cost.rows.has_value() && !cost.cpu.has_value() && !cost.io.has_value()) {
        return "unknown";
    }
    std::ostringstream out;
    out << "{";
    bool needs_separator = false;
    auto append = [&](const std::string& name, double value) {
        if (needs_separator) {
            out << ", ";
        }
        out << name << "=" << value;
        needs_separator = true;
    };
    if (cost.rows.has_value()) {
        append("rows", *cost.rows);
    }
    if (cost.cpu.has_value()) {
        append("cpu", *cost.cpu);
    }
    if (cost.io.has_value()) {
        append("io", *cost.io);
    }
    out << "}";
    return out.str();
}

inline void FormatPhysicalNodeDetail(const PhysicalPlanNode& node, std::ostringstream* out) {
    *out << PhysicalNodeKindName(node.kind);
    *out << (node.lazy ? " [lazy]" : " [eager]");
    *out << "(name=\"" << node.name << "\"";
    if (!node.source.empty()) {
        *out << ", source=\"" << node.source << "\"";
    }
    if (!node.driver.empty()) {
        *out << ", driver=\"" << node.driver << "\"";
    }
    if (!node.logical_prefix.empty()) {
        *out << ", logical_prefix=" << StringList(node.logical_prefix);
    }
    if (!node.optimizer.rbo_rules.empty()) {
        *out << ", rbo=" << StringList(node.optimizer.rbo_rules);
    }
    *out << ", cbo=\"" << node.optimizer.cbo_decision << "\"";
    *out << ", cost=" << CostString(node.optimizer.cost);
    *out << ")";
}

inline void FormatPhysicalPlanTree(const PhysicalPlanNode& node,
                                   const std::string& prefix,
                                   bool is_last,
                                   bool is_root,
                                   std::ostringstream* out) {
    if (is_root) {
        FormatPhysicalNodeDetail(node, out);
    } else {
        *out << prefix << (is_last ? "`- " : "|- ");
        FormatPhysicalNodeDetail(node, out);
    }
    *out << "\n";

    const std::string child_prefix = is_root ? "" : prefix + (is_last ? "   " : "|  ");
    for (size_t i = 0; i < node.inputs.size(); ++i) {
        if (node.inputs[i] != nullptr) {
            FormatPhysicalPlanTree(*node.inputs[i], child_prefix, i + 1 == node.inputs.size(),
                                   false, out);
        }
    }
}

inline std::string FormatPhysicalPlan(const std::shared_ptr<PlanNode>& plan) {
    auto physical = BuildPhysicalPlan(plan);
    if (physical == nullptr) {
        return "<no physical plan>\n";
    }
    std::ostringstream out;
    out << "PhysicalPlan\n";
    FormatPhysicalPlanTree(*physical, "", true, false, &out);
    return out.str();
}

} // namespace pl::flux::plan
