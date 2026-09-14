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
// Created: 2026/09/14 15:20

#pragma once

#include <cstdint>

#include "cpp/pl/prism/syntax/ast.h"

namespace pl::prism::plan {

// Logical plan nodes: a pure structural transform of the AST (see
// docs/design.md §逻辑计划). No catalog binding, no type inference:
// expressions stay in their syntax:: form and are referenced, never copied.
// Like the AST, all nodes are arena-allocated and trivially destructible.
//
// Scope decisions of the current vertical slice:
// - Aggregate calls are NOT symbol-extracted; they remain inside Project
//   projections. Aggregation carries the grouping keys only.
// - Windowed function calls are shallow-collected from the select list into
//   the Window node and ALSO remain inside the projections above it.
// - Aliases of non-table relations are scoping information and are dropped;
//   only TableScan records its alias.
enum class PlanKind : uint16_t {
    kTableScan,
    kValues,
    kUnnest,
    kProject,
    kFilter,
    kAggregation,
    kWindow,
    kSort,
    kLimit,
    kJoin,
    kSetOperation,
};

struct PlanNode {
    PlanKind kind;
    syntax::SourceLocation location;

    template <typename T> [[nodiscard]] bool is() const { return kind == T::kKind; }
    template <typename T> [[nodiscard]] T* as() { return static_cast<T*>(this); }
    template <typename T> [[nodiscard]] const T* as() const { return static_cast<const T*>(this); }

protected:
    PlanNode(PlanKind k, syntax::SourceLocation loc) : kind(k), location(loc) {}
};

// FROM table_name [AS alias [(col, ...)]].
struct TableScan final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kTableScan;
    syntax::AstList<syntax::NamePart> name;
    syntax::NamePart alias;
    bool has_alias;
    syntax::AstList<syntax::NamePart> column_aliases;

    TableScan(syntax::SourceLocation loc,
              syntax::AstList<syntax::NamePart> n,
              syntax::NamePart a,
              bool ha,
              syntax::AstList<syntax::NamePart> ca)
        : PlanNode(kKind, loc), name(n), alias(a), has_alias(ha), column_aliases(ca) {}
};

// VALUES (1, 2), (3, 4) — rows are syntax::Row nodes or bare expressions.
struct Values final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kValues;
    syntax::AstList<syntax::Expression*> rows;

    Values(syntax::SourceLocation loc, syntax::AstList<syntax::Expression*> r)
        : PlanNode(kKind, loc), rows(r) {}
};

// UNNEST(a, b) [WITH ORDINALITY] as a FROM relation.
struct Unnest final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kUnnest;
    syntax::AstList<syntax::Expression*> expressions;
    bool with_ordinality;

    Unnest(syntax::SourceLocation loc, syntax::AstList<syntax::Expression*> e, bool ord)
        : PlanNode(kKind, loc), expressions(e), with_ordinality(ord) {}
};

// One entry of a Project's output list.
struct Projection {
    syntax::Expression* expression = nullptr; // nullptr for star forms
    syntax::NamePart alias{};
    bool has_alias = false;
    bool star = false;                             // `*` or `prefix.*`
    syntax::AstList<syntax::NamePart> star_prefix; // empty for bare `*`
};

// SELECT [DISTINCT] item, ... — the top of a query specification chain.
// source is nullptr for SELECT without FROM.
struct Project final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kProject;
    syntax::AstList<Projection> projections;
    bool distinct;
    PlanNode* source;

    Project(syntax::SourceLocation loc, syntax::AstList<Projection> p, bool d, PlanNode* s)
        : PlanNode(kKind, loc), projections(p), distinct(d), source(s) {}
};

// WHERE / HAVING predicate. source is nullptr for WHERE without FROM.
struct Filter final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kFilter;
    syntax::Expression* predicate;
    PlanNode* source;

    Filter(syntax::SourceLocation loc, syntax::Expression* p, PlanNode* s)
        : PlanNode(kKind, loc), predicate(p), source(s) {}
};

// GROUP BY [DISTINCT] key, ... — grouping keys stay in syntax:: form;
// GROUPING SETS/CUBE/ROLLUP keep their FunctionCall shape.
struct Aggregation final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kAggregation;
    syntax::AstList<syntax::Expression*> grouping_keys;
    bool distinct; // GROUP BY DISTINCT
    PlanNode* source;

    Aggregation(syntax::SourceLocation loc,
                syntax::AstList<syntax::Expression*> k,
                bool d,
                PlanNode* s)
        : PlanNode(kKind, loc), grouping_keys(k), distinct(d), source(s) {}
};

// Windowed aggregates of the select list. functions are the syntax::
// FunctionCall nodes carrying an OVER clause (the window specification rides
// on them); definitions are the named windows of the WINDOW clause that
// OVER name references resolve to.
struct Window final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kWindow;
    syntax::AstList<syntax::Expression*> functions;
    syntax::AstList<syntax::WindowDefinition*> definitions;
    PlanNode* source;

    Window(syntax::SourceLocation loc,
           syntax::AstList<syntax::Expression*> f,
           syntax::AstList<syntax::WindowDefinition*> d,
           PlanNode* s)
        : PlanNode(kKind, loc), functions(f), definitions(d), source(s) {}
};

// ORDER BY item, ...
struct Sort final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kSort;
    syntax::AstList<syntax::SortItem*> items;
    PlanNode* source;

    Sort(syntax::SourceLocation loc, syntax::AstList<syntax::SortItem*> i, PlanNode* s)
        : PlanNode(kKind, loc), items(i), source(s) {}
};

// OFFSET / LIMIT / FETCH FIRST. count is nullptr for LIMIT ALL (and when
// only OFFSET is present).
struct Limit final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kLimit;
    syntax::Expression* count;
    syntax::Expression* offset; // nullptr when absent
    bool with_ties;             // FETCH ... WITH TIES
    PlanNode* source;

    Limit(syntax::SourceLocation loc,
          syntax::Expression* c,
          syntax::Expression* o,
          bool wt,
          PlanNode* s)
        : PlanNode(kKind, loc), count(c), offset(o), with_ties(wt), source(s) {}
};

// [NATURAL] INNER/LEFT/RIGHT/FULL/CROSS JOIN, including the implicit cross
// joins of a multi-relation FROM list.
struct Join final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kJoin;
    syntax::JoinType join_type;
    bool natural;
    PlanNode* left;
    PlanNode* right;
    syntax::Expression* on;                          // nullptr for CROSS/USING
    syntax::AstList<syntax::NamePart> using_columns; // empty unless USING

    Join(syntax::SourceLocation loc,
         syntax::JoinType t,
         bool nat,
         PlanNode* l,
         PlanNode* r,
         syntax::Expression* o,
         syntax::AstList<syntax::NamePart> u)
        : PlanNode(kKind, loc),
          join_type(t),
          natural(nat),
          left(l),
          right(r),
          on(o),
          using_columns(u) {}
};

// UNION / INTERSECT / EXCEPT [ALL | DISTINCT] (left-deep binary, matching
// the AST shape).
struct SetOperation final : PlanNode {
    static constexpr PlanKind kKind = PlanKind::kSetOperation;
    syntax::SetOp op;
    bool all; // false = DISTINCT (also the default)
    PlanNode* left;
    PlanNode* right;

    SetOperation(syntax::SourceLocation loc, syntax::SetOp o, bool a, PlanNode* l, PlanNode* r)
        : PlanNode(kKind, loc), op(o), all(a), left(l), right(r) {}
};

} // namespace pl::prism::plan
