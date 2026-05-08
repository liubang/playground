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
    Distinct,
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
    std::string table;
};

struct RangeSpec {
    std::string start;
    std::optional<std::string> stop;
};

enum class PredicateOp {
    Eq,
    NotEq,
    Lt,
    Lte,
    Gt,
    Gte,
};

enum class PredicateLiteralKind {
    Bool,
    Int,
    UInt,
    Float,
    String,
    Time,
};

struct PredicateLiteral {
    PredicateLiteralKind kind = PredicateLiteralKind::String;
    bool bool_value = false;
    int64_t int_value = 0;
    uint64_t uint_value = 0;
    double float_value = 0.0;
    std::string string_value;
};

struct PredicateSpec {
    PredicateOp op = PredicateOp::Eq;
    std::string column;
    PredicateLiteral literal;
};

struct FilterSpec {
    std::vector<PredicateSpec> predicates;
};

struct ProjectSpec {
    std::vector<std::string> columns;
};

struct RenameSpec {
    std::vector<std::pair<std::string, std::string>> columns;
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

struct GroupSpec {
    std::vector<std::string> columns;
};

enum class AggregateFunction {
    Count,
    Sum,
    Mean,
    Min,
    Max,
};

struct AggregateSpec {
    AggregateFunction fn = AggregateFunction::Count;
    std::string column;
};

struct DistinctSpec {
    std::string column;
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
    FilterSpec filter;
    ProjectSpec project;
    RenameSpec rename;
    LimitSpec limit;
    SortSpec sort;
    GroupSpec group;
    AggregateSpec aggregate;
    DistinctSpec distinct;
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
        case PlanNodeKind::Distinct:
            return "Distinct";
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
                                                std::string table) {
    auto node = std::make_shared<PlanNode>();
    node->kind = PlanNodeKind::SourceScan;
    node->source_scan.source = std::move(source);
    node->source_scan.driver = std::move(driver);
    node->source_scan.dsn = std::move(dsn);
    node->source_scan.table = std::move(table);
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

inline std::shared_ptr<PlanNode> MakeFilter(std::shared_ptr<PlanNode> input,
                                            std::vector<PredicateSpec> predicates) {
    auto node = MakeUnaryNode(PlanNodeKind::Filter, std::move(input));
    node->filter.predicates = std::move(predicates);
    return node;
}

inline std::shared_ptr<PlanNode> MakeProject(std::shared_ptr<PlanNode> input,
                                             std::vector<std::string> columns) {
    auto node = MakeUnaryNode(PlanNodeKind::Project, std::move(input));
    node->project.columns = std::move(columns);
    return node;
}

inline std::shared_ptr<PlanNode> MakeRename(
    std::shared_ptr<PlanNode> input, std::vector<std::pair<std::string, std::string>> columns) {
    auto node = MakeUnaryNode(PlanNodeKind::Rename, std::move(input));
    node->rename.columns = std::move(columns);
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

inline std::shared_ptr<PlanNode> MakeGroup(std::shared_ptr<PlanNode> input,
                                           std::vector<std::string> columns) {
    auto node = MakeUnaryNode(PlanNodeKind::Group, std::move(input));
    node->group.columns = std::move(columns);
    return node;
}

inline std::shared_ptr<PlanNode> MakeAggregate(std::shared_ptr<PlanNode> input,
                                               AggregateFunction fn,
                                               std::string column) {
    auto node = MakeUnaryNode(PlanNodeKind::Aggregate, std::move(input));
    node->aggregate.fn = fn;
    node->aggregate.column = std::move(column);
    return node;
}

inline std::shared_ptr<PlanNode> MakeDistinct(std::shared_ptr<PlanNode> input, std::string column) {
    auto node = MakeUnaryNode(PlanNodeKind::Distinct, std::move(input));
    node->distinct.column = std::move(column);
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

inline std::string PredicateOpName(PredicateOp op) {
    switch (op) {
        case PredicateOp::Eq:
            return "==";
        case PredicateOp::NotEq:
            return "!=";
        case PredicateOp::Lt:
            return "<";
        case PredicateOp::Lte:
            return "<=";
        case PredicateOp::Gt:
            return ">";
        case PredicateOp::Gte:
            return ">=";
    }
    return "==";
}

inline std::string PredicateLiteralString(const PredicateLiteral& literal) {
    switch (literal.kind) {
        case PredicateLiteralKind::Bool:
            return literal.bool_value ? "true" : "false";
        case PredicateLiteralKind::Int:
            return std::to_string(literal.int_value);
        case PredicateLiteralKind::UInt:
            return std::to_string(literal.uint_value);
        case PredicateLiteralKind::Float: {
            std::ostringstream out;
            out << literal.float_value;
            return out.str();
        }
        case PredicateLiteralKind::String:
            return "\"" + literal.string_value + "\"";
        case PredicateLiteralKind::Time:
            return literal.string_value;
    }
    return "";
}

inline std::string PredicateSpecString(const PredicateSpec& predicate) {
    return predicate.column + " " + PredicateOpName(predicate.op) + " " +
           PredicateLiteralString(predicate.literal);
}

inline std::string PredicateListString(const std::vector<PredicateSpec>& predicates) {
    std::ostringstream out;
    for (size_t i = 0; i < predicates.size(); ++i) {
        if (i != 0) {
            out << " and ";
        }
        out << PredicateSpecString(predicates[i]);
    }
    return out.str();
}

enum class PushdownState {
    SqliteScan,
    SqlitePushdown,
    MaterializeBarrier,
    Memory,
};

inline bool IsSqliteSourceScan(const PlanNode& node) {
    return node.kind == PlanNodeKind::SourceScan && node.source_scan.source == "datasource" &&
           node.source_scan.driver == "sqlite";
}

inline bool IsPushableUnaryNode(const PlanNode& node) {
    switch (node.kind) {
        case PlanNodeKind::Range:
        case PlanNodeKind::Project:
        case PlanNodeKind::Rename:
        case PlanNodeKind::Limit:
        case PlanNodeKind::Sort:
        case PlanNodeKind::Group:
        case PlanNodeKind::Aggregate:
        case PlanNodeKind::Distinct:
            return true;
        case PlanNodeKind::Filter:
            return !node.filter.predicates.empty();
        default:
            return false;
    }
}

inline PushdownState AnalyzePushdownState(const PlanNode& node) {
    if (IsSqliteSourceScan(node)) {
        return PushdownState::SqliteScan;
    }
    if (node.kind == PlanNodeKind::Materialize) {
        return PushdownState::MaterializeBarrier;
    }
    if (!IsPushableUnaryNode(node) || node.inputs.size() != 1 || node.inputs[0] == nullptr) {
        return PushdownState::Memory;
    }
    const PushdownState input_state = AnalyzePushdownState(*node.inputs[0]);
    if (input_state == PushdownState::SqliteScan || input_state == PushdownState::SqlitePushdown) {
        return PushdownState::SqlitePushdown;
    }
    return PushdownState::Memory;
}

inline void FormatPushdownState(const PlanNode& node, std::ostringstream* out) {
    switch (AnalyzePushdownState(node)) {
        case PushdownState::SqliteScan:
            *out << " [sqlite scan]";
            return;
        case PushdownState::SqlitePushdown:
            *out << " [sqlite pushdown";
            if (node.kind == PlanNodeKind::Filter && !node.filter.predicates.empty()) {
                *out << ": " << PredicateListString(node.filter.predicates);
            }
            *out << "]";
            return;
        case PushdownState::MaterializeBarrier:
            *out << " [barrier";
            if (!node.materialize.reason.empty()) {
                *out << ": " << node.materialize.reason;
            }
            *out << "]";
            return;
        case PushdownState::Memory:
            *out << " [memory]";
            return;
    }
}

inline void FormatPlanNode(const PlanNode& node, size_t depth, std::ostringstream* out) {
    for (size_t i = 0; i < depth; ++i) {
        *out << "  ";
    }
    *out << PlanNodeKindName(node.kind);
    FormatPushdownState(node, out);
    if (node.kind == PlanNodeKind::SourceScan) {
        *out << "(source=\"" << node.source_scan.source << "\", driver=\""
             << node.source_scan.driver << "\", table=\"" << node.source_scan.table << "\")";
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
