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
    OutputSink,
    ConnectorScan,
    MemoryScan,
    MemoryOperator,
    LocalHashJoin,
    Exchange,
    Materialize,
};

struct CostEstimate {
    std::optional<double> rows;
    std::optional<double> cpu;
    std::optional<double> io;
};

struct OptimizerTrace {
    std::vector<std::string> rbo_rules;
    std::vector<std::string> cbo_alternatives;
    std::string cbo_decision = "not-run";
    CostEstimate cost;
};

struct PhysicalPlanNode {
    PhysicalNodeKind kind = PhysicalNodeKind::MemoryOperator;
    std::string name;
    std::string operator_name;
    std::string source;
    std::string driver;
    std::string table;
    std::string connector_handle;
    std::optional<size_t> split_count;
    std::vector<std::string> logical_prefix;
    bool lazy = false;
    OptimizerTrace optimizer;
    std::vector<std::shared_ptr<PhysicalPlanNode>> inputs;
};

inline std::string PhysicalNodeKindName(PhysicalNodeKind kind) {
    switch (kind) {
        case PhysicalNodeKind::OutputSink:
            return "OutputSink";
        case PhysicalNodeKind::ConnectorScan:
            return "ConnectorScan";
        case PhysicalNodeKind::MemoryScan:
            return "MemoryScan";
        case PhysicalNodeKind::MemoryOperator:
            return "MemoryOperator";
        case PhysicalNodeKind::LocalHashJoin:
            return "LocalHashJoin";
        case PhysicalNodeKind::Exchange:
            return "Exchange";
        case PhysicalNodeKind::Materialize:
            return "Materialize";
    }
    return "MemoryOperator";
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

inline void FormatPhysicalNodeHeader(const PhysicalPlanNode& node, std::ostringstream* out) {
    *out << PhysicalNodeKindName(node.kind);
    *out << (node.lazy ? " [lazy]" : " [eager]");
}

inline void FormatPhysicalScalarProperty(const std::string& indent,
                                         const std::string& name,
                                         const std::string& value,
                                         std::ostringstream* out) {
    *out << indent << name << ": " << value << "\n";
}

inline void FormatPhysicalStringProperty(const std::string& indent,
                                         const std::string& name,
                                         const std::string& value,
                                         std::ostringstream* out) {
    *out << indent << name << ": \"" << value << "\"\n";
}

inline void FormatPhysicalListProperty(const std::string& indent,
                                       const std::string& name,
                                       const std::vector<std::string>& values,
                                       std::ostringstream* out) {
    if (values.empty()) {
        return;
    }
    *out << indent << name << ":\n";
    for (const auto& value : values) {
        *out << indent << "  - " << value << "\n";
    }
}

inline void FormatPhysicalNodeProperties(const PhysicalPlanNode& node,
                                         const std::string& indent,
                                         std::ostringstream* out) {
    if (!node.name.empty()) {
        FormatPhysicalStringProperty(indent, "name", node.name, out);
    }
    if (!node.operator_name.empty()) {
        FormatPhysicalStringProperty(indent, "operator", node.operator_name, out);
    }
    if (!node.source.empty()) {
        FormatPhysicalStringProperty(indent, "source", node.source, out);
    }
    if (!node.driver.empty()) {
        FormatPhysicalStringProperty(indent, "driver", node.driver, out);
    }
    if (!node.table.empty()) {
        FormatPhysicalStringProperty(indent, "table", node.table, out);
    }
    if (!node.connector_handle.empty()) {
        FormatPhysicalStringProperty(indent, "handle", node.connector_handle, out);
    }
    if (node.split_count.has_value()) {
        FormatPhysicalScalarProperty(indent, "splits", std::to_string(*node.split_count), out);
    }
    FormatPhysicalListProperty(indent, "logical_prefix", node.logical_prefix, out);
    FormatPhysicalListProperty(indent, "rbo", node.optimizer.rbo_rules, out);
    FormatPhysicalStringProperty(indent, "cbo", node.optimizer.cbo_decision, out);
    FormatPhysicalScalarProperty(indent, "cost", CostString(node.optimizer.cost), out);
    FormatPhysicalListProperty(indent, "alternatives", node.optimizer.cbo_alternatives, out);
}

inline void FormatPhysicalPlanTree(const PhysicalPlanNode& node,
                                   const std::string& prefix,
                                   bool is_last,
                                   bool is_root,
                                   std::ostringstream* out) {
    if (is_root) {
        FormatPhysicalNodeHeader(node, out);
    } else {
        *out << prefix << (is_last ? "`- " : "|- ");
        FormatPhysicalNodeHeader(node, out);
    }
    *out << "\n";

    const std::string child_prefix = is_root ? "" : prefix + (is_last ? "   " : "|  ");
    const std::string property_indent = child_prefix + "  ";
    FormatPhysicalNodeProperties(node, property_indent, out);
    for (size_t i = 0; i < node.inputs.size(); ++i) {
        if (node.inputs[i] != nullptr) {
            FormatPhysicalPlanTree(*node.inputs[i], child_prefix, i + 1 == node.inputs.size(),
                                   false, out);
        }
    }
}

inline std::string FormatPhysicalPlan(const std::shared_ptr<PhysicalPlanNode>& physical) {
    if (physical == nullptr) {
        return "<no physical plan>\n";
    }
    std::ostringstream out;
    out << "PhysicalPlan\n";
    FormatPhysicalPlanTree(*physical, "", true, false, &out);
    return out.str();
}

} // namespace pl::flux::plan
