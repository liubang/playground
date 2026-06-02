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

#include <sstream>
#include <vector>

#include "cpp/pl/flux/connector/connector_registry.h"
#include "cpp/pl/flux/connector/connector_runtime.h"
#include "cpp/pl/flux/optimizer/cbo.h"
#include "cpp/pl/flux/optimizer/rbo.h"
#include "cpp/pl/flux/plan/physical_plan.h"

namespace pl::flux::optimizer {
namespace {

std::string FormatCboAlternative(const PlanAlternative& alternative) {
    std::ostringstream out;
    out << alternative.name;
    if (alternative.chosen) {
        out << "*";
    }
    out << ":" << plan::CostString(alternative.cost);
    return out.str();
}

void ApplyCboTrace(const CboPlanResult& cbo_result, plan::PhysicalPlanNode* physical) {
    physical->optimizer.rbo_rules = AppliedRuleNames(cbo_result.rbo_result);
    physical->optimizer.cbo_alternatives.clear();
    physical->optimizer.cbo_alternatives.reserve(cbo_result.alternatives.size());
    for (const auto& alternative : cbo_result.alternatives) {
        physical->optimizer.cbo_alternatives.push_back(FormatCboAlternative(alternative));
    }
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
    } else if (node.kind == plan::PlanNodeKind::Join) {
        *out << "(method=\"" << plan::JoinMethodName(node.join().method)
             << "\", on=" << plan::StringList(node.join().on) << ", build=\""
             << plan::JoinBuildSideName(node.join().build_side) << "\", distribution=\""
             << plan::JoinDistributionKindName(node.join().distribution.kind)
             << "\", partitions=" << node.join().distribution.partitions;
        if (!node.join().distribution.heavy_hitters.empty()) {
            *out << ", heavy_hitters=" << plan::StringList(node.join().distribution.heavy_hitters);
        }
        *out << ")";
    } else if (node.kind == plan::PlanNodeKind::Exchange) {
        *out << "(kind=\"" << plan::ExchangeKindName(node.exchange().kind) << "\"";
        if (!node.exchange().partition_keys.empty()) {
            *out << ", keys=" << plan::StringList(node.exchange().partition_keys);
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
            FormatLogicalPlanTree(
                *node.inputs[i], child_prefix, i + 1 == node.inputs.size(), false, out);
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

const plan::SourceScanSpec* FindSourceScan(const plan::PlanNode& node) {
    if (node.kind == plan::PlanNodeKind::SourceScan) {
        return &node.source_scan();
    }
    for (const auto& input : node.inputs) {
        if (input != nullptr) {
            const auto* source = FindSourceScan(*input);
            if (source != nullptr) {
                return source;
            }
        }
    }
    return nullptr;
}

void ApplyConnectorRuntimeDetail(const std::shared_ptr<plan::PlanNode>& node,
                                 plan::PhysicalPlanNode* physical) {
    const auto* source = FindSourceScan(*node);
    if (source == nullptr) {
        return;
    }
    physical->source = source->source;
    physical->driver = source->driver;
    physical->table = source->table;
    physical->connector_handle = source->source + ":" + source->driver + ":" + source->table;

    auto optimized_or = DefaultRuleBasedOptimizer().Optimize(node);
    if (!optimized_or.ok() || !optimized_or->pushdown_plan.has_value()) {
        return;
    }
    const auto& pushdown = *optimized_or->pushdown_plan;
    connector::SourceSpec spec{.source = source->source,
                               .driver = source->driver,
                               .dsn = source->dsn,
                               .table = source->table};
    auto runtime_or = connector::ConnectorRegistry::Global().CreateRuntime(spec);
    if (!runtime_or.ok()) {
        return;
    }
    auto handle_or = (*runtime_or)->metadata->GetTableHandle(spec);
    if (!handle_or.ok()) {
        return;
    }
    auto splits_or = (*runtime_or)->split_manager->GetSplits(*handle_or, pushdown.request);
    if (splits_or.ok()) {
        physical->split_count = splits_or->size();
    }
}

std::shared_ptr<plan::PhysicalPlanNode> MakeConnectorPhysicalPlan(
    const std::shared_ptr<plan::PlanNode>& node) {
    auto physical = std::make_shared<plan::PhysicalPlanNode>();
    physical->kind = plan::PhysicalNodeKind::ConnectorScan;
    physical->name = "connector scan";
    physical->operator_name = "ConnectorScanOperator";
    physical->source = PushdownSourceName(*node).value_or("source");
    physical->driver = physical->source;
    physical->lazy = true;
    CollectLogicalPrefix(*node, &physical->logical_prefix);
    ApplyConnectorRuntimeDetail(node, physical.get());
    ApplyCboTraceForPlan(node, physical.get());
    return physical;
}

std::string OperatorNameForPlanNode(plan::PlanNodeKind kind) {
    switch (kind) {
        case plan::PlanNodeKind::Range:
            return "RangeOperator";
        case plan::PlanNodeKind::Filter:
            return "FilterOperator";
        case plan::PlanNodeKind::Project:
            return "ProjectOperator";
        case plan::PlanNodeKind::Rename:
            return "RenameOperator";
        case plan::PlanNodeKind::Limit:
            return "LimitOperator";
        case plan::PlanNodeKind::Sort:
            return "SortOperator";
        case plan::PlanNodeKind::Group:
            return "GroupOperator";
        case plan::PlanNodeKind::Aggregate:
            return "AggregateOperator";
        case plan::PlanNodeKind::Distinct:
            return "DistinctOperator";
        case plan::PlanNodeKind::Join:
            return "LocalHashJoinOperator";
        case plan::PlanNodeKind::Exchange:
            return "ExchangeOperator";
        case plan::PlanNodeKind::Materialize:
            return "MaterializeOperator";
        default:
            return "MemoryOperator";
    }
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
    if (node->kind == plan::PlanNodeKind::Materialize) {
        physical->kind = plan::PhysicalNodeKind::Materialize;
    } else if (node->kind == plan::PlanNodeKind::Join) {
        physical->kind = plan::PhysicalNodeKind::LocalHashJoin;
    } else if (node->kind == plan::PlanNodeKind::Exchange) {
        physical->kind = plan::PhysicalNodeKind::Exchange;
    } else {
        physical->kind = plan::PhysicalNodeKind::MemoryOperator;
    }
    physical->name = plan::PlanNodeKindName(node->kind);
    physical->operator_name = OperatorNameForPlanNode(node->kind);
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
        physical->operator_name = "MemoryScanOperator";
        physical->source = node->source_scan().source;
        physical->driver = node->source_scan().driver;
    }
    return physical;
}

std::shared_ptr<plan::PhysicalPlanNode> BuildOutputPhysicalPlan(
    const std::shared_ptr<plan::PlanNode>& node) {
    if (node == nullptr) {
        return nullptr;
    }
    auto output = std::make_shared<plan::PhysicalPlanNode>();
    output->kind = plan::PhysicalNodeKind::OutputSink;
    output->name = "output";
    output->operator_name = "OutputOperator";
    output->lazy = false;
    output->inputs.push_back(BuildPhysicalPlan(node));
    return output;
}

std::string EscapeMermaidLabel(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "<br/>";
                break;
            case '|':
                out << "&#124;";
                break;
            default:
                out << c;
                break;
        }
    }
    return out.str();
}

std::string LogicalNodeLabel(const plan::PlanNode& node) {
    std::ostringstream label;
    FormatLogicalNodeDetail(node, &label);
    return label.str();
}

std::string PhysicalNodeLabel(const plan::PhysicalPlanNode& node) {
    std::ostringstream label;
    label << plan::PhysicalNodeKindName(node.kind) << (node.lazy ? " [lazy]" : " [eager]");
    if (!node.operator_name.empty()) {
        label << "\noperator: " << node.operator_name;
    }
    if (!node.source.empty()) {
        label << "\nsource: " << node.source;
    }
    if (!node.table.empty()) {
        label << "\ntable: " << node.table;
    }
    if (node.split_count.has_value()) {
        label << "\nsplits: " << *node.split_count;
    }
    if (!node.optimizer.cbo_decision.empty()) {
        label << "\ncbo: " << node.optimizer.cbo_decision;
    }
    return label.str();
}

void AppendMermaidNode(const std::string& id, const std::string& label, std::ostringstream* out) {
    *out << "  " << id << "[\"" << EscapeMermaidLabel(label) << "\"]\n";
}

void AppendLogicalMermaidTree(const plan::PlanNode& node,
                              size_t* next_id,
                              std::ostringstream* out,
                              std::string* node_id) {
    *node_id = "n" + std::to_string((*next_id)++);
    AppendMermaidNode(*node_id, LogicalNodeLabel(node), out);
    for (const auto& input : node.inputs) {
        if (input == nullptr) {
            continue;
        }
        std::string child_id;
        AppendLogicalMermaidTree(*input, next_id, out, &child_id);
        *out << "  " << *node_id << " --> " << child_id << "\n";
    }
}

void AppendPhysicalMermaidTree(const plan::PhysicalPlanNode& node,
                               size_t* next_id,
                               std::ostringstream* out,
                               std::string* node_id) {
    *node_id = "n" + std::to_string((*next_id)++);
    AppendMermaidNode(*node_id, PhysicalNodeLabel(node), out);
    for (const auto& input : node.inputs) {
        if (input == nullptr) {
            continue;
        }
        std::string child_id;
        AppendPhysicalMermaidTree(*input, next_id, out, &child_id);
        *out << "  " << *node_id << " --> " << child_id << "\n";
    }
}

std::string FormatLogicalPlanMermaidOnly(const std::shared_ptr<plan::PlanNode>& plan) {
    std::ostringstream out;
    out << "flowchart TD\n";
    if (plan == nullptr) {
        AppendMermaidNode("n0", "<no plan>", &out);
        return out.str();
    }
    size_t next_id = 0;
    std::string root_id;
    AppendLogicalMermaidTree(*plan, &next_id, &out, &root_id);
    return out.str();
}

std::string FormatPhysicalPlanMermaidOnly(const std::shared_ptr<plan::PhysicalPlanNode>& physical) {
    std::ostringstream out;
    out << "flowchart TD\n";
    if (physical == nullptr) {
        AppendMermaidNode("n0", "<no physical plan>", &out);
        return out.str();
    }
    size_t next_id = 0;
    std::string root_id;
    AppendPhysicalMermaidTree(*physical, &next_id, &out, &root_id);
    return out.str();
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
    }
    return out;
}

std::string FormatLogicalPlanMermaid(const std::shared_ptr<plan::PlanNode>& plan) {
    return FormatLogicalPlanMermaidOnly(plan);
}

std::string FormatOptimizedLogicalPlan(const std::shared_ptr<plan::PlanNode>& plan) {
    if (plan == nullptr) {
        return "OptimizedLogicalPlan\n<no plan>\n";
    }
    auto cbo_or = DefaultCostBasedOptimizer().OptimizeWithTrace(plan);
    if (!cbo_or.ok()) {
        return cbo_or.status().ToString() + "\n";
    }
    const auto& optimized = cbo_or->rbo_result;
    std::ostringstream out;
    out << "OptimizedLogicalPlan\n";
    out << FormatLogicalPlanOnly(optimized.plan);
    const auto rules = AppliedRuleNames(optimized);
    if (!rules.empty()) {
        out << "RBO(rules=" << plan::StringList(rules) << ")\n";
    }
    if (auto summary = SourcePushdownSummary(optimized); summary.has_value()) {
        out << *summary;
    }
    return out.str();
}

std::string FormatOptimizedLogicalPlanMermaid(const std::shared_ptr<plan::PlanNode>& plan) {
    if (plan == nullptr) {
        return "flowchart TD\n  n0[\"<no plan>\"]\n";
    }
    auto cbo_or = DefaultCostBasedOptimizer().OptimizeWithTrace(plan);
    if (!cbo_or.ok()) {
        std::ostringstream out;
        out << "flowchart TD\n";
        AppendMermaidNode("n0", cbo_or.status().ToString(), &out);
        return out.str();
    }
    return FormatLogicalPlanMermaidOnly(cbo_or->rbo_result.plan);
}

std::string FormatPhysicalPlan(const std::shared_ptr<plan::PlanNode>& plan) {
    if (plan == nullptr) {
        return "<no physical plan>\n";
    }
    auto cbo_or = DefaultCostBasedOptimizer().OptimizeWithTrace(plan);
    if (!cbo_or.ok()) {
        return cbo_or.status().ToString() + "\n";
    }
    return plan::FormatPhysicalPlan(BuildOutputPhysicalPlan(cbo_or->rbo_result.plan));
}

std::string FormatPhysicalPlanMermaid(const std::shared_ptr<plan::PlanNode>& plan) {
    if (plan == nullptr) {
        return "flowchart TD\n  n0[\"<no physical plan>\"]\n";
    }
    auto cbo_or = DefaultCostBasedOptimizer().OptimizeWithTrace(plan);
    if (!cbo_or.ok()) {
        std::ostringstream out;
        out << "flowchart TD\n";
        AppendMermaidNode("n0", cbo_or.status().ToString(), &out);
        return out.str();
    }
    return FormatPhysicalPlanMermaidOnly(BuildOutputPhysicalPlan(cbo_or->rbo_result.plan));
}

} // namespace pl::flux::optimizer
