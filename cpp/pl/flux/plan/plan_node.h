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
#include <variant>
#include <vector>

namespace pl::flux::plan {

// ---------------------------------------------------------------------------
// Node kind enum (retained for fast switch-based dispatch).
// ---------------------------------------------------------------------------

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
    Exchange,
    Yield,
    Materialize,
};

// ---------------------------------------------------------------------------
// Spec structs (unchanged from before).
// ---------------------------------------------------------------------------

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

enum class JoinMethod {
    Inner,
    Left,
    Right,
    Full,
};

struct JoinSpec {
    std::vector<std::string> on;
    std::string left_name = "left";
    std::string right_name = "right";
    JoinMethod method = JoinMethod::Inner;
};

enum class ExchangeKind {
    Gather,
    Repartition,
};

struct ExchangeSpec {
    ExchangeKind kind = ExchangeKind::Gather;
    std::vector<std::string> partition_keys;
};

struct MaterializeSpec {
    std::string reason;
    std::string builtin;
};

// ---------------------------------------------------------------------------
// NodeSpec variant — only the active spec occupies memory.
// std::monostate is used for node kinds that carry no extra data
// (Map, Window, Union, Yield).
// ---------------------------------------------------------------------------

using NodeSpec = std::variant<std::monostate,
                              SourceScanSpec,
                              RangeSpec,
                              FilterSpec,
                              ProjectSpec,
                              RenameSpec,
                              LimitSpec,
                              SortSpec,
                              GroupSpec,
                              AggregateSpec,
                              DistinctSpec,
                              JoinSpec,
                              ExchangeSpec,
                              MaterializeSpec>;

// ---------------------------------------------------------------------------
// PlanNode — the core logical plan tree node.
// ---------------------------------------------------------------------------

struct PlanNode {
    PlanNodeKind kind = PlanNodeKind::Materialize;
    std::vector<std::shared_ptr<PlanNode>> inputs;
    NodeSpec spec = MaterializeSpec{};

    // -- Typed accessors (const & mutable) ----------------------------------
    // These provide the same ergonomics as direct field access.

    [[nodiscard]] const SourceScanSpec& source_scan() const {
        return std::get<SourceScanSpec>(spec);
    }
    SourceScanSpec& source_scan() { return std::get<SourceScanSpec>(spec); }

    [[nodiscard]] const RangeSpec& range() const { return std::get<RangeSpec>(spec); }
    RangeSpec& range() { return std::get<RangeSpec>(spec); }

    [[nodiscard]] const FilterSpec& filter() const { return std::get<FilterSpec>(spec); }
    FilterSpec& filter() { return std::get<FilterSpec>(spec); }

    [[nodiscard]] const ProjectSpec& project() const { return std::get<ProjectSpec>(spec); }
    ProjectSpec& project() { return std::get<ProjectSpec>(spec); }

    [[nodiscard]] const RenameSpec& rename() const { return std::get<RenameSpec>(spec); }
    RenameSpec& rename() { return std::get<RenameSpec>(spec); }

    [[nodiscard]] const LimitSpec& limit() const { return std::get<LimitSpec>(spec); }
    LimitSpec& limit() { return std::get<LimitSpec>(spec); }

    [[nodiscard]] const SortSpec& sort() const { return std::get<SortSpec>(spec); }
    SortSpec& sort() { return std::get<SortSpec>(spec); }

    [[nodiscard]] const GroupSpec& group() const { return std::get<GroupSpec>(spec); }
    GroupSpec& group() { return std::get<GroupSpec>(spec); }

    [[nodiscard]] const AggregateSpec& aggregate() const { return std::get<AggregateSpec>(spec); }
    AggregateSpec& aggregate() { return std::get<AggregateSpec>(spec); }

    [[nodiscard]] const DistinctSpec& distinct() const { return std::get<DistinctSpec>(spec); }
    DistinctSpec& distinct() { return std::get<DistinctSpec>(spec); }

    [[nodiscard]] const JoinSpec& join() const { return std::get<JoinSpec>(spec); }
    JoinSpec& join() { return std::get<JoinSpec>(spec); }

    [[nodiscard]] const ExchangeSpec& exchange() const { return std::get<ExchangeSpec>(spec); }
    ExchangeSpec& exchange() { return std::get<ExchangeSpec>(spec); }

    [[nodiscard]] const MaterializeSpec& materialize() const {
        return std::get<MaterializeSpec>(spec);
    }
    MaterializeSpec& materialize() { return std::get<MaterializeSpec>(spec); }
};

// ---------------------------------------------------------------------------
// PlanNodeKindName
// ---------------------------------------------------------------------------

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
        case PlanNodeKind::Exchange:
            return "Exchange";
        case PlanNodeKind::Yield:
            return "Yield";
        case PlanNodeKind::Materialize:
            return "Materialize";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

inline std::shared_ptr<PlanNode> MakeSourceScan(std::string source,
                                                std::string driver,
                                                std::string dsn,
                                                std::string table) {
    auto node = std::make_shared<PlanNode>();
    node->kind = PlanNodeKind::SourceScan;
    node->spec = SourceScanSpec{.source = std::move(source),
                                .driver = std::move(driver),
                                .dsn = std::move(dsn),
                                .table = std::move(table)};
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
    node->spec = RangeSpec{.start = std::move(start), .stop = std::move(stop)};
    return node;
}

inline std::shared_ptr<PlanNode> MakeFilter(std::shared_ptr<PlanNode> input,
                                            std::vector<PredicateSpec> predicates) {
    auto node = MakeUnaryNode(PlanNodeKind::Filter, std::move(input));
    node->spec = FilterSpec{std::move(predicates)};
    return node;
}

inline std::shared_ptr<PlanNode> MakeProject(std::shared_ptr<PlanNode> input,
                                             std::vector<std::string> columns) {
    auto node = MakeUnaryNode(PlanNodeKind::Project, std::move(input));
    node->spec = ProjectSpec{std::move(columns)};
    return node;
}

inline std::shared_ptr<PlanNode> MakeRename(
    std::shared_ptr<PlanNode> input, std::vector<std::pair<std::string, std::string>> columns) {
    auto node = MakeUnaryNode(PlanNodeKind::Rename, std::move(input));
    node->spec = RenameSpec{std::move(columns)};
    return node;
}

inline std::shared_ptr<PlanNode> MakeLimit(std::shared_ptr<PlanNode> input,
                                           int64_t n,
                                           int64_t offset) {
    auto node = MakeUnaryNode(PlanNodeKind::Limit, std::move(input));
    node->spec = LimitSpec{.n = n, .offset = offset};
    return node;
}

inline std::shared_ptr<PlanNode> MakeSort(std::shared_ptr<PlanNode> input,
                                          std::vector<SortKey> keys) {
    auto node = MakeUnaryNode(PlanNodeKind::Sort, std::move(input));
    node->spec = SortSpec{std::move(keys)};
    return node;
}

inline std::shared_ptr<PlanNode> MakeGroup(std::shared_ptr<PlanNode> input,
                                           std::vector<std::string> columns) {
    auto node = MakeUnaryNode(PlanNodeKind::Group, std::move(input));
    node->spec = GroupSpec{std::move(columns)};
    return node;
}

inline std::shared_ptr<PlanNode> MakeAggregate(std::shared_ptr<PlanNode> input,
                                               AggregateFunction fn,
                                               std::string column) {
    auto node = MakeUnaryNode(PlanNodeKind::Aggregate, std::move(input));
    node->spec = AggregateSpec{.fn = fn, .column = std::move(column)};
    return node;
}

inline std::shared_ptr<PlanNode> MakeDistinct(std::shared_ptr<PlanNode> input, std::string column) {
    auto node = MakeUnaryNode(PlanNodeKind::Distinct, std::move(input));
    node->spec = DistinctSpec{std::move(column)};
    return node;
}

inline std::shared_ptr<PlanNode> MakeJoin(std::shared_ptr<PlanNode> left,
                                          std::shared_ptr<PlanNode> right,
                                          std::vector<std::string> on,
                                          JoinMethod method = JoinMethod::Inner,
                                          std::string left_name = "left",
                                          std::string right_name = "right") {
    auto node = std::make_shared<PlanNode>();
    node->kind = PlanNodeKind::Join;
    node->inputs.push_back(std::move(left));
    node->inputs.push_back(std::move(right));
    node->spec = JoinSpec{
        .on = std::move(on),
        .left_name = std::move(left_name),
        .right_name = std::move(right_name),
        .method = method,
    };
    return node;
}

inline std::shared_ptr<PlanNode> MakeExchange(std::shared_ptr<PlanNode> input,
                                              ExchangeKind kind,
                                              std::vector<std::string> partition_keys = {}) {
    auto node = MakeUnaryNode(PlanNodeKind::Exchange, std::move(input));
    node->spec = ExchangeSpec{
        .kind = kind,
        .partition_keys = std::move(partition_keys),
    };
    return node;
}

inline std::string JoinMethodName(JoinMethod method) {
    switch (method) {
        case JoinMethod::Inner:
            return "inner";
        case JoinMethod::Left:
            return "left";
        case JoinMethod::Right:
            return "right";
        case JoinMethod::Full:
            return "full";
    }
    return "inner";
}

inline std::string ExchangeKindName(ExchangeKind kind) {
    switch (kind) {
        case ExchangeKind::Gather:
            return "gather";
        case ExchangeKind::Repartition:
            return "repartition";
    }
    return "gather";
}

inline std::shared_ptr<PlanNode> MakeMaterializeBarrier(std::shared_ptr<PlanNode> input,
                                                        std::string reason,
                                                        std::string builtin) {
    auto node = MakeUnaryNode(PlanNodeKind::Materialize, std::move(input));
    node->spec = MaterializeSpec{.reason = std::move(reason), .builtin = std::move(builtin)};
    return node;
}

inline std::shared_ptr<PlanNode> MakeMaterializeBarrier(
    std::vector<std::shared_ptr<PlanNode>> inputs, std::string reason, std::string builtin) {
    auto node = std::make_shared<PlanNode>();
    node->kind = PlanNodeKind::Materialize;
    node->inputs = std::move(inputs);
    node->spec = MaterializeSpec{.reason = std::move(reason), .builtin = std::move(builtin)};
    return node;
}

// ---------------------------------------------------------------------------
// Predicate / formatting utilities
// ---------------------------------------------------------------------------

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

inline std::string StringList(const std::vector<std::string>& items) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << items[i];
    }
    out << "]";
    return out.str();
}

// ---------------------------------------------------------------------------
// Plan tree formatting
// ---------------------------------------------------------------------------

inline void FormatNodeDetail(const PlanNode& node, std::ostringstream* out) {
    *out << PlanNodeKindName(node.kind);
    if (node.kind == PlanNodeKind::SourceScan) {
        const auto& ss = node.source_scan();
        *out << "(source=\"" << ss.source << "\", driver=\"" << ss.driver << "\", table=\""
             << ss.table << "\")";
    } else if (node.kind == PlanNodeKind::Materialize) {
        const auto& ms = node.materialize();
        if (!ms.reason.empty() || !ms.builtin.empty()) {
            *out << "(";
            bool needs_separator = false;
            if (!ms.reason.empty()) {
                *out << "reason=\"" << ms.reason << "\"";
                needs_separator = true;
            }
            if (!ms.builtin.empty()) {
                if (needs_separator) {
                    *out << ", ";
                }
                *out << "builtin=\"" << ms.builtin << "\"";
            }
            *out << ")";
        }
    } else if (node.kind == PlanNodeKind::Join) {
        const auto& join = node.join();
        *out << "(method=\"" << JoinMethodName(join.method) << "\", on="
             << StringList(join.on) << ")";
    } else if (node.kind == PlanNodeKind::Exchange) {
        const auto& exchange = node.exchange();
        *out << "(kind=\"" << ExchangeKindName(exchange.kind) << "\"";
        if (!exchange.partition_keys.empty()) {
            *out << ", keys=" << StringList(exchange.partition_keys);
        }
        *out << ")";
    }
}

inline void FormatPlanTree(const PlanNode& node,
                           const std::string& prefix,
                           bool is_last,
                           bool is_root,
                           std::ostringstream* out) {
    // 输出当前节点的连接线和内容
    if (is_root) {
        FormatNodeDetail(node, out);
    } else {
        *out << prefix << (is_last ? "`- " : "|- ");
        FormatNodeDetail(node, out);
    }
    *out << "\n";

    // 递归输出子节点
    const std::string child_prefix = is_root ? "" : prefix + (is_last ? "   " : "|  ");
    for (size_t i = 0; i < node.inputs.size(); ++i) {
        if (node.inputs[i] != nullptr) {
            FormatPlanTree(*node.inputs[i], child_prefix, i + 1 == node.inputs.size(), false, out);
        }
    }
}

inline std::string FormatPlan(const std::shared_ptr<PlanNode>& plan) {
    if (plan == nullptr) {
        return "<no plan>";
    }
    std::ostringstream out;
    FormatPlanTree(*plan, "", true, true, &out);
    return out.str();
}

} // namespace pl::flux::plan
