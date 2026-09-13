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
// Created: 2026/09/13 13:24

#pragma once

#include <cstdint>
#include <string_view>

namespace pl::prism::syntax {

struct SourceLocation {
    uint32_t offset = 0;
    uint32_t line = 1;
    uint32_t column = 1;
};

// A list of AST elements stored contiguously in the arena. Trivially
// copyable; the backing array is owned by the arena.
template <typename T> struct AstList {
    T* data = nullptr;
    uint32_t size = 0;

    [[nodiscard]] bool empty() const { return size == 0; }
    [[nodiscard]] T* begin() const { return data; }
    [[nodiscard]] T* end() const { return data + size; }
    [[nodiscard]] T& operator[](uint32_t i) const { return data[i]; }
};

// A (possibly quoted) name fragment, e.g. one part of a.b.c. For quoted
// parts, text holds the raw source text including the quotes.
struct NamePart {
    std::string_view text;
    bool quoted = false;
};

enum class NodeKind : uint16_t {
    // Expressions
    kIdentifier,
    kDereference,
    kNumberLiteral,
    kStringLiteral,
    kBooleanLiteral,
    kNullLiteral,
    kIntervalLiteral,
    kTypedLiteral,
    kArithmeticBinary,
    kArithmeticUnary,
    kComparison,
    kIsNull,
    kBetween,
    kInPredicate,
    kInList,
    kLike,
    kLogicalBinary,
    kNot,
    kExists,
    kSimpleCase,
    kSearchedCase,
    kWhenClause,
    kCast,
    kFunctionCall,
    kWindow,
    kWindowFrame,
    kSubscript,
    kLambda,
    kRow,
    kArrayConstructor,
    kSubqueryExpression,
    kTypeName,
    // Select items
    kSingleColumn,
    kAllColumns,
    // Relations
    kTable,
    kAliasedRelation,
    kJoin,
    kLateral,
    kTableSubquery,
    kTableSample,
    kUnnest,
    kValues,
    // Query nodes
    kQuery,
    kQuerySpecification,
    kSetOperation,
    kWith,
    kWithQuery,
    kSortItem,
    // Statements
    kExplain,
};

enum class ArithmeticOp : uint8_t {
    kAdd,
    kSubtract,
    kMultiply,
    kDivide,
    kModulus,
    kConcatenate,
};

enum class ComparisonOp : uint8_t {
    kEqual,
    kNotEqual,
    kLessThan,
    kGreaterThan,
    kLessThanOrEqual,
    kGreaterThanOrEqual,
    kIsDistinctFrom,
};

enum class LogicalOp : uint8_t { kAnd, kOr };

enum class JoinType : uint8_t { kInner, kLeft, kRight, kFull, kCross };

enum class SetOp : uint8_t { kUnion, kIntersect, kExcept };

enum class Ordering : uint8_t { kUnspecified, kAsc, kDesc };

enum class NullOrdering : uint8_t { kUnspecified, kFirst, kLast };

enum class FrameType : uint8_t { kRows, kRange, kGroups };

enum class FrameBoundType : uint8_t {
    kUnboundedPreceding,
    kPreceding,
    kCurrentRow,
    kFollowing,
    kUnboundedFollowing,
};

enum class TimeZoneSpec : uint8_t { kNone, kWith, kWithout };

enum class SampleType : uint8_t { kBernoulli, kSystem };

struct Query;
struct SortItem;
struct Window;
struct WindowFrame;
struct TypeName;
struct WhenClause;
struct With;

// All nodes are arena-allocated and trivially destructible: they only hold
// pointers, string_views and AstLists. The arena frees them in bulk.
struct Node {
    NodeKind kind;
    SourceLocation location;

    template <typename T> [[nodiscard]] bool is() const { return kind == T::kKind; }
    template <typename T> [[nodiscard]] T* as() { return static_cast<T*>(this); }
    template <typename T> [[nodiscard]] const T* as() const { return static_cast<const T*>(this); }

protected:
    Node(NodeKind k, SourceLocation loc) : kind(k), location(loc) {}
};

struct Expression : Node {
protected:
    Expression(NodeKind k, SourceLocation loc) : Node(k, loc) {}
};

// Expression nodes.

struct Identifier final : Expression {
    static constexpr NodeKind kKind = NodeKind::kIdentifier;
    std::string_view name;
    bool quoted;

    Identifier(SourceLocation loc, std::string_view n, bool q)
        : Expression(kKind, loc), name(n), quoted(q) {}
};

// base.field, e.g. a.b.c is Dereference(Dereference(a, b), c).
struct DereferenceExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kDereference;
    Expression* base;
    std::string_view field;
    bool field_quoted;

    DereferenceExpression(SourceLocation loc, Expression* b, std::string_view f, bool q)
        : Expression(kKind, loc), base(b), field(f), field_quoted(q) {}
};

struct NumberLiteral final : Expression {
    static constexpr NodeKind kKind = NodeKind::kNumberLiteral;
    std::string_view value; // raw source text

    NumberLiteral(SourceLocation loc, std::string_view v) : Expression(kKind, loc), value(v) {}
};

struct StringLiteral final : Expression {
    static constexpr NodeKind kKind = NodeKind::kStringLiteral;
    std::string_view value; // raw source text, including quotes and any prefix

    StringLiteral(SourceLocation loc, std::string_view v) : Expression(kKind, loc), value(v) {}
};

struct BooleanLiteral final : Expression {
    static constexpr NodeKind kKind = NodeKind::kBooleanLiteral;
    bool value;

    BooleanLiteral(SourceLocation loc, bool v) : Expression(kKind, loc), value(v) {}
};

struct NullLiteral final : Expression {
    static constexpr NodeKind kKind = NodeKind::kNullLiteral;

    explicit NullLiteral(SourceLocation loc) : Expression(kKind, loc) {}
};

// INTERVAL [-] 'value' unit [TO unit]
struct IntervalLiteral final : Expression {
    static constexpr NodeKind kKind = NodeKind::kIntervalLiteral;
    bool negative;
    std::string_view value; // raw string token text
    std::string_view from_unit;
    std::string_view to_unit; // empty when absent

    IntervalLiteral(SourceLocation loc,
                    bool neg,
                    std::string_view v,
                    std::string_view from,
                    std::string_view to)
        : Expression(kKind, loc), negative(neg), value(v), from_unit(from), to_unit(to) {}
};

// DATE 'x' / TIME 'x' / TIMESTAMP 'x'
struct TypedLiteral final : Expression {
    static constexpr NodeKind kKind = NodeKind::kTypedLiteral;
    std::string_view type_name; // raw keyword text
    std::string_view value;     // raw string token text

    TypedLiteral(SourceLocation loc, std::string_view t, std::string_view v)
        : Expression(kKind, loc), type_name(t), value(v) {}
};

struct ArithmeticBinaryExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kArithmeticBinary;
    ArithmeticOp op;
    Expression* left;
    Expression* right;

    ArithmeticBinaryExpression(SourceLocation loc, ArithmeticOp o, Expression* l, Expression* r)
        : Expression(kKind, loc), op(o), left(l), right(r) {}
};

struct ArithmeticUnaryExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kArithmeticUnary;
    bool negative;
    Expression* value;

    ArithmeticUnaryExpression(SourceLocation loc, bool neg, Expression* v)
        : Expression(kKind, loc), negative(neg), value(v) {}
};

struct ComparisonExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kComparison;
    ComparisonOp op;
    Expression* left;
    Expression* right;
    bool negated; // only meaningful for kIsDistinctFrom (IS NOT DISTINCT FROM)

    ComparisonExpression(SourceLocation loc, ComparisonOp o, Expression* l, Expression* r, bool n)
        : Expression(kKind, loc), op(o), left(l), right(r), negated(n) {}
};

struct IsNullPredicate final : Expression {
    static constexpr NodeKind kKind = NodeKind::kIsNull;
    Expression* value;
    bool negated;

    IsNullPredicate(SourceLocation loc, Expression* v, bool n)
        : Expression(kKind, loc), value(v), negated(n) {}
};

struct BetweenPredicate final : Expression {
    static constexpr NodeKind kKind = NodeKind::kBetween;
    Expression* value;
    Expression* min;
    Expression* max;
    bool negated;

    BetweenPredicate(SourceLocation loc, Expression* v, Expression* lo, Expression* hi, bool n)
        : Expression(kKind, loc), value(v), min(lo), max(hi), negated(n) {}
};

struct InListExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kInList;
    AstList<Expression*> items;

    InListExpression(SourceLocation loc, AstList<Expression*> i)
        : Expression(kKind, loc), items(i) {}
};

struct InPredicate final : Expression {
    static constexpr NodeKind kKind = NodeKind::kInPredicate;
    Expression* value;
    Expression* value_list; // InListExpression or SubqueryExpression
    bool negated;

    InPredicate(SourceLocation loc, Expression* v, Expression* vl, bool n)
        : Expression(kKind, loc), value(v), value_list(vl), negated(n) {}
};

struct LikePredicate final : Expression {
    static constexpr NodeKind kKind = NodeKind::kLike;
    Expression* value;
    Expression* pattern;
    Expression* escape;    // nullptr when absent
    bool case_insensitive; // ILIKE
    bool negated;

    LikePredicate(SourceLocation loc, Expression* v, Expression* p, Expression* e, bool ci, bool n)
        : Expression(kKind, loc),
          value(v),
          pattern(p),
          escape(e),
          case_insensitive(ci),
          negated(n) {}
};

struct LogicalBinaryExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kLogicalBinary;
    LogicalOp op;
    Expression* left;
    Expression* right;

    LogicalBinaryExpression(SourceLocation loc, LogicalOp o, Expression* l, Expression* r)
        : Expression(kKind, loc), op(o), left(l), right(r) {}
};

struct NotExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kNot;
    Expression* value;

    NotExpression(SourceLocation loc, Expression* v) : Expression(kKind, loc), value(v) {}
};

struct ExistsPredicate final : Expression {
    static constexpr NodeKind kKind = NodeKind::kExists;
    Query* query;

    ExistsPredicate(SourceLocation loc, Query* q) : Expression(kKind, loc), query(q) {}
};

struct WhenClause final : Node {
    static constexpr NodeKind kKind = NodeKind::kWhenClause;
    Expression* when;
    Expression* result;

    WhenClause(SourceLocation loc, Expression* w, Expression* r)
        : Node(kKind, loc), when(w), result(r) {}
};

// CASE operand WHEN ... END
struct SimpleCaseExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kSimpleCase;
    Expression* operand;
    AstList<WhenClause*> when_clauses;
    Expression* else_result; // nullptr when absent

    SimpleCaseExpression(SourceLocation loc, Expression* op, AstList<WhenClause*> w, Expression* e)
        : Expression(kKind, loc), operand(op), when_clauses(w), else_result(e) {}
};

// CASE WHEN ... END
struct SearchedCaseExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kSearchedCase;
    AstList<WhenClause*> when_clauses;
    Expression* else_result; // nullptr when absent

    SearchedCaseExpression(SourceLocation loc, AstList<WhenClause*> w, Expression* e)
        : Expression(kKind, loc), when_clauses(w), else_result(e) {}
};

// Structured type reference: VARCHAR(10), DECIMAL(10,2), ARRAY(BIGINT),
// MAP(VARCHAR, INT), ROW(a INT, b VARCHAR), TIMESTAMP WITH TIME ZONE.
struct TypeName final : Node {
    static constexpr NodeKind kKind = NodeKind::kTypeName;
    std::string_view name; // raw source text of the type keyword(s)
    AstList<TypeName*> type_args;
    AstList<std::string_view> num_args;
    AstList<NamePart> field_names; // parallel to type_args for ROW (empty text = anonymous)
    TimeZoneSpec time_zone;

    TypeName(SourceLocation loc,
             std::string_view n,
             AstList<TypeName*> ta,
             AstList<std::string_view> na,
             AstList<NamePart> fn,
             TimeZoneSpec tz)
        : Node(kKind, loc), name(n), type_args(ta), num_args(na), field_names(fn), time_zone(tz) {}
};

struct Cast final : Expression {
    static constexpr NodeKind kKind = NodeKind::kCast;
    Expression* expression;
    TypeName* type;
    bool try_cast;

    Cast(SourceLocation loc, Expression* e, TypeName* t, bool try_)
        : Expression(kKind, loc), expression(e), type(t), try_cast(try_) {}
};

struct FunctionCall final : Expression {
    static constexpr NodeKind kKind = NodeKind::kFunctionCall;
    AstList<NamePart> name;
    bool distinct;
    bool wildcard; // f(*)
    AstList<Expression*> args;
    Window* window; // nullptr when no OVER clause

    FunctionCall(SourceLocation loc,
                 AstList<NamePart> n,
                 bool d,
                 bool w,
                 AstList<Expression*> a,
                 Window* win)
        : Expression(kKind, loc), name(n), distinct(d), wildcard(w), args(a), window(win) {}
};

struct FrameBound {
    FrameBoundType type = FrameBoundType::kCurrentRow;
    Expression* value = nullptr; // only for kPreceding / kFollowing
};

struct WindowFrame final : Node {
    static constexpr NodeKind kKind = NodeKind::kWindowFrame;
    FrameType frame_type;
    FrameBound start;
    FrameBound end;
    bool has_end;

    WindowFrame(SourceLocation loc, FrameType t, FrameBound s, FrameBound e, bool he)
        : Node(kKind, loc), frame_type(t), start(s), end(e), has_end(he) {}
};

struct Window final : Node {
    static constexpr NodeKind kKind = NodeKind::kWindow;
    AstList<Expression*> partition_by;
    AstList<SortItem*> order_by;
    WindowFrame* frame; // nullptr when absent

    Window(SourceLocation loc, AstList<Expression*> p, AstList<SortItem*> o, WindowFrame* f)
        : Node(kKind, loc), partition_by(p), order_by(o), frame(f) {}
};

struct SubscriptExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kSubscript;
    Expression* base;
    Expression* index;

    SubscriptExpression(SourceLocation loc, Expression* b, Expression* i)
        : Expression(kKind, loc), base(b), index(i) {}
};

struct LambdaExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kLambda;
    AstList<NamePart> parameters;
    Expression* body;

    LambdaExpression(SourceLocation loc, AstList<NamePart> p, Expression* b)
        : Expression(kKind, loc), parameters(p), body(b) {}
};

struct Row final : Expression {
    static constexpr NodeKind kKind = NodeKind::kRow;
    AstList<Expression*> items;

    Row(SourceLocation loc, AstList<Expression*> i) : Expression(kKind, loc), items(i) {}
};

// ARRAY[a, b, ...]
struct ArrayConstructor final : Expression {
    static constexpr NodeKind kKind = NodeKind::kArrayConstructor;
    AstList<Expression*> items;

    ArrayConstructor(SourceLocation loc, AstList<Expression*> i)
        : Expression(kKind, loc), items(i) {}
};

// (SELECT ...) as a scalar expression
struct SubqueryExpression final : Expression {
    static constexpr NodeKind kKind = NodeKind::kSubqueryExpression;
    Query* query;

    SubqueryExpression(SourceLocation loc, Query* q) : Expression(kKind, loc), query(q) {}
};

// Select items.

struct SelectItem : Node {
protected:
    SelectItem(NodeKind k, SourceLocation loc) : Node(k, loc) {}
};

// expression [AS alias]
struct SingleColumn final : SelectItem {
    static constexpr NodeKind kKind = NodeKind::kSingleColumn;
    Expression* expression;
    NamePart alias;
    bool has_alias;

    SingleColumn(SourceLocation loc, Expression* e, NamePart a, bool ha)
        : SelectItem(kKind, loc), expression(e), alias(a), has_alias(ha) {}
};

// * or prefix.*
struct AllColumns final : SelectItem {
    static constexpr NodeKind kKind = NodeKind::kAllColumns;
    AstList<NamePart> prefix; // empty for bare *

    AllColumns(SourceLocation loc, AstList<NamePart> p) : SelectItem(kKind, loc), prefix(p) {}
};

// Relation nodes.

struct Relation : Node {
protected:
    Relation(NodeKind k, SourceLocation loc) : Node(k, loc) {}
};

struct Table final : Relation {
    static constexpr NodeKind kKind = NodeKind::kTable;
    AstList<NamePart> name;

    Table(SourceLocation loc, AstList<NamePart> n) : Relation(kKind, loc), name(n) {}
};

// relation [AS] alias [(col1, col2, ...)]
struct AliasedRelation final : Relation {
    static constexpr NodeKind kKind = NodeKind::kAliasedRelation;
    Relation* relation;
    NamePart alias;
    AstList<NamePart> column_aliases;

    AliasedRelation(SourceLocation loc, Relation* r, NamePart a, AstList<NamePart> ca)
        : Relation(kKind, loc), relation(r), alias(a), column_aliases(ca) {}
};

struct Join final : Relation {
    static constexpr NodeKind kKind = NodeKind::kJoin;
    JoinType join_type;
    bool natural;
    Relation* left;
    Relation* right;
    Expression* on;                  // nullptr when USING or CROSS
    AstList<NamePart> using_columns; // empty when ON or CROSS

    Join(SourceLocation loc,
         JoinType t,
         bool nat,
         Relation* l,
         Relation* r,
         Expression* o,
         AstList<NamePart> u)
        : Relation(kKind, loc),
          join_type(t),
          natural(nat),
          left(l),
          right(r),
          on(o),
          using_columns(u) {}
};

// LATERAL (query)
struct Lateral final : Relation {
    static constexpr NodeKind kKind = NodeKind::kLateral;
    Query* query;

    Lateral(SourceLocation loc, Query* q) : Relation(kKind, loc), query(q) {}
};

struct TableSubquery final : Relation {
    static constexpr NodeKind kKind = NodeKind::kTableSubquery;
    Query* query;

    TableSubquery(SourceLocation loc, Query* q) : Relation(kKind, loc), query(q) {}
};

// relation TABLESAMPLE BERNOULLI|SYSTEM (percentage)
struct TableSample final : Relation {
    static constexpr NodeKind kKind = NodeKind::kTableSample;
    Relation* relation;
    SampleType sample_type;
    Expression* percentage;

    TableSample(SourceLocation loc, Relation* r, SampleType t, Expression* p)
        : Relation(kKind, loc), relation(r), sample_type(t), percentage(p) {}
};

// UNNEST(a, b) [WITH ORDINALITY]
struct Unnest final : Relation {
    static constexpr NodeKind kKind = NodeKind::kUnnest;
    AstList<Expression*> expressions;
    bool with_ordinality;

    Unnest(SourceLocation loc, AstList<Expression*> e, bool ord)
        : Relation(kKind, loc), expressions(e), with_ordinality(ord) {}
};

// VALUES (1, 2), (3, 4) — each row is a Row node or a bare expression.
struct Values final : Relation {
    static constexpr NodeKind kKind = NodeKind::kValues;
    AstList<Expression*> rows;

    Values(SourceLocation loc, AstList<Expression*> r) : Relation(kKind, loc), rows(r) {}
};

// Query nodes and statements.

struct SortItem final : Node {
    static constexpr NodeKind kKind = NodeKind::kSortItem;
    Expression* sort_key;
    Ordering ordering;
    NullOrdering null_ordering;

    SortItem(SourceLocation loc, Expression* k, Ordering o, NullOrdering n)
        : Node(kKind, loc), sort_key(k), ordering(o), null_ordering(n) {}
};

struct WithQuery final : Node {
    static constexpr NodeKind kKind = NodeKind::kWithQuery;
    NamePart name;
    AstList<NamePart> column_aliases;
    Query* query;

    WithQuery(SourceLocation loc, NamePart n, AstList<NamePart> ca, Query* q)
        : Node(kKind, loc), name(n), column_aliases(ca), query(q) {}
};

struct With final : Node {
    static constexpr NodeKind kKind = NodeKind::kWith;
    bool recursive;
    AstList<WithQuery*> queries;

    With(SourceLocation loc, bool r, AstList<WithQuery*> q)
        : Node(kKind, loc), recursive(r), queries(q) {}
};

struct QuerySpecification final : Node {
    static constexpr NodeKind kKind = NodeKind::kQuerySpecification;
    bool distinct;
    AstList<SelectItem*> select_items;
    AstList<Relation*> from; // empty when no FROM clause
    Expression* where;       // nullptr when absent
    Expression* having;      // nullptr when absent
    AstList<Expression*> group_by;

    QuerySpecification(SourceLocation loc,
                       bool d,
                       AstList<SelectItem*> si,
                       AstList<Relation*> f,
                       Expression* w,
                       Expression* h,
                       AstList<Expression*> g)
        : Node(kKind, loc),
          distinct(d),
          select_items(si),
          from(f),
          where(w),
          having(h),
          group_by(g) {}
};

struct SetOperation final : Node {
    static constexpr NodeKind kKind = NodeKind::kSetOperation;
    SetOp op;
    Node* left;
    Node* right;
    bool all; // false = DISTINCT (also the default when unspecified)

    SetOperation(SourceLocation loc, SetOp o, Node* l, Node* r, bool a)
        : Node(kKind, loc), op(o), left(l), right(r), all(a) {}
};

struct Query final : Node {
    static constexpr NodeKind kKind = NodeKind::kQuery;
    With* with; // nullptr when absent
    Node* body; // QuerySpecification / SetOperation / Values / Query
    AstList<SortItem*> order_by;
    Expression* offset; // nullptr when absent
    Expression* limit;  // nullptr when absent or LIMIT ALL

    Query(SourceLocation loc,
          With* w,
          Node* b,
          AstList<SortItem*> o,
          Expression* off,
          Expression* lim)
        : Node(kKind, loc), with(w), body(b), order_by(o), offset(off), limit(lim) {}
};

struct ExplainOption {
    NamePart name;
    NamePart value;
};

// EXPLAIN [ANALYZE] [VERBOSE] [(option value, ...)] statement
struct Explain final : Node {
    static constexpr NodeKind kKind = NodeKind::kExplain;
    bool analyze;
    bool verbose;
    AstList<ExplainOption> options;
    Node* statement;

    Explain(SourceLocation loc, bool a, bool v, AstList<ExplainOption> o, Node* s)
        : Node(kKind, loc), analyze(a), verbose(v), options(o), statement(s) {}
};

} // namespace pl::prism::syntax
