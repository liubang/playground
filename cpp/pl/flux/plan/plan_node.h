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
// Created: 2026/05/07 00:35

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pl::flux::plan {

enum class PlanNodeKind {
    SourceScan,
    Range,
    Filter,
    Project,
    Rename,
    Map,
    Limit,
    Sort,
    Group,
    Aggregate,
    Window,
    Join,
    Union,
    Yield,
    Materialize,
};

struct SourceScanSpec {
    std::string source;
    std::string driver;
    std::string dsn;
    std::string query;
};

struct RangeSpec {
    std::string start;
    std::optional<std::string> stop;
};

struct ProjectSpec {
    std::vector<std::string> columns;
};

struct LimitSpec {
    int64_t n = 0;
    int64_t offset = 0;
};

struct SortKey {
    std::string column;
    bool desc = false;
};

struct SortSpec {
    std::vector<SortKey> keys;
};

struct MaterializeSpec {
    std::string reason;
    std::string builtin;
};

struct PlanNode {
    PlanNodeKind kind = PlanNodeKind::Materialize;
    std::vector<std::shared_ptr<PlanNode>> inputs;
    SourceScanSpec source_scan;
    RangeSpec range;
    ProjectSpec project;
    LimitSpec limit;
    SortSpec sort;
    MaterializeSpec materialize;
};

inline std::string PlanNodeKindName(PlanNodeKind kind) {
    switch (kind) {
        case PlanNodeKind::SourceScan:
            return "SourceScan";
        case PlanNodeKind::Range:
            return "Range";
        case PlanNodeKind::Filter:
            return "Filter";
        case PlanNodeKind::Project:
            return "Project";
        case PlanNodeKind::Rename:
            return "Rename";
        case PlanNodeKind::Map:
            return "Map";
        case PlanNodeKind::Limit:
            return "Limit";
        case PlanNodeKind::Sort:
            return "Sort";
        case PlanNodeKind::Group:
            return "Group";
        case PlanNodeKind::Aggregate:
            return "Aggregate";
        case PlanNodeKind::Window:
            return "Window";
        case PlanNodeKind::Join:
            return "Join";
        case PlanNodeKind::Union:
            return "Union";
        case PlanNodeKind::Yield:
            return "Yield";
        case PlanNodeKind::Materialize:
            return "Materialize";
    }
    return "Unknown";
}

inline std::shared_ptr<PlanNode> MakeSourceScan(std::string source,
                                                std::string driver,
                                                std::string dsn,
                                                std::string query) {
    auto node = std::make_shared<PlanNode>();
    node->kind = PlanNodeKind::SourceScan;
    node->source_scan.source = std::move(source);
    node->source_scan.driver = std::move(driver);
    node->source_scan.dsn = std::move(dsn);
    node->source_scan.query = std::move(query);
    return node;
}

inline std::shared_ptr<PlanNode> MakeUnaryNode(PlanNodeKind kind, std::shared_ptr<PlanNode> input) {
    auto node = std::make_shared<PlanNode>();
    node->kind = kind;
    node->inputs.push_back(std::move(input));
    return node;
}

inline std::shared_ptr<PlanNode> MakeRange(std::shared_ptr<PlanNode> input,
                                           std::string start,
                                           std::optional<std::string> stop) {
    auto node = MakeUnaryNode(PlanNodeKind::Range, std::move(input));
    node->range.start = std::move(start);
    node->range.stop = std::move(stop);
    return node;
}

inline std::shared_ptr<PlanNode> MakeProject(std::shared_ptr<PlanNode> input,
                                             std::vector<std::string> columns) {
    auto node = MakeUnaryNode(PlanNodeKind::Project, std::move(input));
    node->project.columns = std::move(columns);
    return node;
}

inline std::shared_ptr<PlanNode> MakeLimit(std::shared_ptr<PlanNode> input,
                                           int64_t n,
                                           int64_t offset) {
    auto node = MakeUnaryNode(PlanNodeKind::Limit, std::move(input));
    node->limit.n = n;
    node->limit.offset = offset;
    return node;
}

inline std::shared_ptr<PlanNode> MakeSort(std::shared_ptr<PlanNode> input,
                                          std::vector<SortKey> keys) {
    auto node = MakeUnaryNode(PlanNodeKind::Sort, std::move(input));
    node->sort.keys = std::move(keys);
    return node;
}

inline std::shared_ptr<PlanNode> MakeMaterializeBarrier(std::shared_ptr<PlanNode> input,
                                                        std::string reason,
                                                        std::string builtin) {
    auto node = MakeUnaryNode(PlanNodeKind::Materialize, std::move(input));
    node->materialize.reason = std::move(reason);
    node->materialize.builtin = std::move(builtin);
    return node;
}

inline std::shared_ptr<PlanNode> MakeMaterializeBarrier(
    std::vector<std::shared_ptr<PlanNode>> inputs, std::string reason, std::string builtin) {
    auto node = std::make_shared<PlanNode>();
    node->kind = PlanNodeKind::Materialize;
    node->inputs = std::move(inputs);
    node->materialize.reason = std::move(reason);
    node->materialize.builtin = std::move(builtin);
    return node;
}

inline void FormatPlanNode(const PlanNode& node, size_t depth, std::ostringstream* out) {
    for (size_t i = 0; i < depth; ++i) {
        *out << "  ";
    }
    *out << PlanNodeKindName(node.kind);
    if (node.kind == PlanNodeKind::SourceScan) {
        *out << "(source=\"" << node.source_scan.source << "\", driver=\""
             << node.source_scan.driver << "\", query=\"" << node.source_scan.query << "\")";
    } else if (node.kind == PlanNodeKind::Materialize &&
               (!node.materialize.reason.empty() || !node.materialize.builtin.empty())) {
        *out << "(";
        bool needs_separator = false;
        if (!node.materialize.reason.empty()) {
            *out << "reason=\"" << node.materialize.reason << "\"";
            needs_separator = true;
        }
        if (!node.materialize.builtin.empty()) {
            if (needs_separator) {
                *out << ", ";
            }
            *out << "builtin=\"" << node.materialize.builtin << "\"";
        }
        *out << ")";
    }
    *out << "\n";
    for (const auto& input : node.inputs) {
        if (input != nullptr) {
            FormatPlanNode(*input, depth + 1, out);
        }
    }
}

inline std::string FormatPlan(const std::shared_ptr<PlanNode>& plan) {
    if (plan == nullptr) {
        return "<no plan>";
    }
    std::ostringstream out;
    FormatPlanNode(*plan, 0, &out);
    return out.str();
}

} // namespace pl::flux::plan
