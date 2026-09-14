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
// Created: 2026/09/14 13:44

#include "cpp/pl/prism/printer/sql_printer.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pl::prism::printer {

using syntax::AliasedRelation;
using syntax::AllColumns;
using syntax::ArithmeticBinaryExpression;
using syntax::ArithmeticOp;
using syntax::ArithmeticUnaryExpression;
using syntax::ArrayConstructor;
using syntax::AstList;
using syntax::AtTimeZone;
using syntax::BetweenPredicate;
using syntax::BetweenSymmetry;
using syntax::BooleanTestPredicate;
using syntax::BooleanTestType;
using syntax::Cast;
using syntax::ComparisonExpression;
using syntax::ComparisonOp;
using syntax::DereferenceExpression;
using syntax::Explain;
using syntax::Expression;
using syntax::FrameBound;
using syntax::FrameBoundType;
using syntax::FrameType;
using syntax::FunctionCall;
using syntax::GroupingOperation;
using syntax::Identifier;
using syntax::InListExpression;
using syntax::InPredicate;
using syntax::IntervalLiteral;
using syntax::IsNullPredicate;
using syntax::Join;
using syntax::JoinType;
using syntax::LambdaExpression;
using syntax::Lateral;
using syntax::LikePredicate;
using syntax::ListaggExpression;
using syntax::LogicalBinaryExpression;
using syntax::LogicalOp;
using syntax::MatchPredicate;
using syntax::MatchType;
using syntax::NamePart;
using syntax::Node;
using syntax::NodeKind;
using syntax::NotExpression;
using syntax::NullOrdering;
using syntax::NumberLiteral;
using syntax::Ordering;
using syntax::OverflowBehavior;
using syntax::OverflowCount;
using syntax::OverlayExpression;
using syntax::PositionExpression;
using syntax::QuantifiedComparisonExpression;
using syntax::Quantifier;
using syntax::Query;
using syntax::QuerySpecification;
using syntax::Row;
using syntax::SampleType;
using syntax::SearchedCaseExpression;
using syntax::SetOp;
using syntax::SetOperation;
using syntax::SimpleCaseExpression;
using syntax::SortItem;
using syntax::StringLiteral;
using syntax::SubqueryExpression;
using syntax::SubscriptExpression;
using syntax::SubstringExpression;
using syntax::Table;
using syntax::TableSample;
using syntax::TableSubquery;
using syntax::TimeZoneSpec;
using syntax::TrimExpression;
using syntax::TrimSpec;
using syntax::TypedLiteral;
using syntax::TypeName;
using syntax::Unnest;
using syntax::Values;
using syntax::WhenClause;
using syntax::Window;
using syntax::WindowDefinition;
using syntax::WindowFrame;
using syntax::With;
using syntax::WithQuery;

namespace {

// Operator precedence ladder for minimal parenthesization, mirroring the
// parser's precedence chain: a child is parenthesized iff its own precedence
// is strictly lower than the context requires. Left-associative chains pass
// min = prec to the left subtree and min = prec + 1 to the right subtree,
// which preserves the exact tree shape across a re-parse.
constexpr uint8_t kPrecOr = 1;
constexpr uint8_t kPrecAnd = 2;
constexpr uint8_t kPrecNot = 3;
constexpr uint8_t kPrecPredicate = 4;
constexpr uint8_t kPrecValue = 5; // predicate operands (concat level and above)
constexpr uint8_t kPrecConcat = 6;
constexpr uint8_t kPrecAdd = 7;
constexpr uint8_t kPrecMul = 8;
constexpr uint8_t kPrecUnary = 9;
constexpr uint8_t kPrecPostfix = 10;
constexpr uint8_t kPrecPrimary = 11;

const char* comparison_op_symbol(ComparisonOp op) {
    switch (op) {
        case ComparisonOp::kEqual:
            return "=";
        case ComparisonOp::kNotEqual:
            return "<>";
        case ComparisonOp::kLessThan:
            return "<";
        case ComparisonOp::kGreaterThan:
            return ">";
        case ComparisonOp::kLessThanOrEqual:
            return "<=";
        case ComparisonOp::kGreaterThanOrEqual:
            return ">=";
        case ComparisonOp::kIsDistinctFrom:
            return "IS DISTINCT FROM";
    }
    return "?";
}

uint8_t precedence(const Expression* e) {
    switch (e->kind) {
        case NodeKind::kLambda:
            // A lambda swallows the rest of the expression as its body, so it
            // only stays bare where a full expression is accepted.
            return 0;
        case NodeKind::kLogicalBinary:
            return e->as<LogicalBinaryExpression>()->op == LogicalOp::kOr ? kPrecOr : kPrecAnd;
        case NodeKind::kNot:
            return kPrecNot;
        case NodeKind::kComparison:
        case NodeKind::kIsNull:
        case NodeKind::kBetween:
        case NodeKind::kInPredicate:
        case NodeKind::kLike:
        case NodeKind::kBooleanTest:
        case NodeKind::kQuantifiedComparison:
        case NodeKind::kMatchPredicate:
            return kPrecPredicate;
        case NodeKind::kArithmeticBinary:
            switch (e->as<ArithmeticBinaryExpression>()->op) {
                case ArithmeticOp::kConcatenate:
                    return kPrecConcat;
                case ArithmeticOp::kAdd:
                case ArithmeticOp::kSubtract:
                    return kPrecAdd;
                case ArithmeticOp::kMultiply:
                case ArithmeticOp::kDivide:
                case ArithmeticOp::kModulus:
                    return kPrecMul;
            }
            return kPrecPrimary;
        case NodeKind::kArithmeticUnary:
            return kPrecUnary;
        case NodeKind::kSubscript:
        case NodeKind::kDereference:
        case NodeKind::kAtTimeZone:
            return kPrecPostfix;
        default:
            return kPrecPrimary;
    }
}

class Printer {
public:
    explicit Printer(const dialect::Dialect& dialect) : dialect_(dialect) {}

    std::string run(const Node* node) {
        visit(node);
        return std::move(out_);
    }

private:
    void append(std::string_view s) { out_.append(s); }
    void space() { out_.push_back(' '); }

    void expr(const Expression* e, uint8_t min_prec) {
        const bool parens = precedence(e) < min_prec;
        if (parens) {
            out_.push_back('(');
        }
        visit_expr(e);
        if (parens) {
            out_.push_back(')');
        }
    }

    // Identifiers and names.

    void identifier(std::string_view text, bool quoted) {
        if (!quoted || text.size() < 2 || dialect_.identifier_quote == text.front()) {
            append(text);
            return;
        }
        // Re-quote a Trino-style "quoted" identifier with the dialect's quote
        // character, decoding the "" escape and escaping the target quote.
        const char quote = dialect_.identifier_quote;
        out_.push_back(quote);
        for (size_t i = 1; i + 1 < text.size(); ++i) {
            const char c = text[i];
            if (c == '"' && text[i + 1] == '"') {
                ++i;
                out_.push_back('"');
                continue;
            }
            if (c == quote) {
                out_.push_back(quote);
            }
            out_.push_back(c);
        }
        out_.push_back(quote);
    }

    void name_part(const NamePart& part) { identifier(part.text, part.quoted); }

    void name_parts(const AstList<NamePart>& parts) {
        for (uint32_t i = 0; i < parts.size; ++i) {
            if (i > 0) {
                out_.push_back('.');
            }
            name_part(parts[i]);
        }
    }

    void name_list(const AstList<NamePart>& parts) {
        for (uint32_t i = 0; i < parts.size; ++i) {
            if (i > 0) {
                append(", ");
            }
            name_part(parts[i]);
        }
    }

    // Types.

    void type(const TypeName* t) {
        append(t->name);
        if (!t->num_args.empty()) {
            out_.push_back('(');
            for (uint32_t i = 0; i < t->num_args.size; ++i) {
                if (i > 0) {
                    append(", ");
                }
                append(t->num_args[i]);
            }
            out_.push_back(')');
        }
        if (!t->type_args.empty()) {
            out_.push_back('(');
            for (uint32_t i = 0; i < t->type_args.size; ++i) {
                if (i > 0) {
                    append(", ");
                }
                if (!t->field_names.empty() && !t->field_names[i].text.empty()) {
                    name_part(t->field_names[i]);
                    space();
                }
                type(t->type_args[i]);
            }
            out_.push_back(')');
        }
        if (t->time_zone == TimeZoneSpec::kWith) {
            append(" WITH TIME ZONE");
        } else if (t->time_zone == TimeZoneSpec::kWithout) {
            append(" WITHOUT TIME ZONE");
        }
    }

    // Sort items and windows.

    void sort_item(const SortItem* item) {
        expr(item->sort_key, 0);
        if (item->ordering == Ordering::kAsc) {
            append(" ASC");
        } else if (item->ordering == Ordering::kDesc) {
            append(" DESC");
        }
        if (item->null_ordering == NullOrdering::kFirst) {
            append(" NULLS FIRST");
        } else if (item->null_ordering == NullOrdering::kLast) {
            append(" NULLS LAST");
        }
    }

    void sort_list(const AstList<SortItem*>& items) {
        for (uint32_t i = 0; i < items.size; ++i) {
            if (i > 0) {
                append(", ");
            }
            sort_item(items[i]);
        }
    }

    void frame_bound(const FrameBound& bound) {
        switch (bound.type) {
            case FrameBoundType::kUnboundedPreceding:
                append("UNBOUNDED PRECEDING");
                break;
            case FrameBoundType::kPreceding:
                expr(bound.value, 0);
                append(" PRECEDING");
                break;
            case FrameBoundType::kCurrentRow:
                append("CURRENT ROW");
                break;
            case FrameBoundType::kFollowing:
                expr(bound.value, 0);
                append(" FOLLOWING");
                break;
            case FrameBoundType::kUnboundedFollowing:
                append("UNBOUNDED FOLLOWING");
                break;
        }
    }

    void frame(const WindowFrame* f) {
        switch (f->frame_type) {
            case FrameType::kRows:
                append("ROWS");
                break;
            case FrameType::kRange:
                append("RANGE");
                break;
            case FrameType::kGroups:
                append("GROUPS");
                break;
        }
        if (f->has_end) {
            append(" BETWEEN ");
            frame_bound(f->start);
            append(" AND ");
            frame_bound(f->end);
        } else {
            space();
            frame_bound(f->start);
        }
    }

    // The body of a window specification, without the enclosing parens.
    void window_contents(const Window* w) {
        bool first = true;
        const auto sep = [&]() {
            if (!first) {
                space();
            }
            first = false;
        };
        if (w->has_existing_window) {
            sep();
            name_part(w->existing_window);
        }
        if (!w->partition_by.empty()) {
            sep();
            append("PARTITION BY ");
            for (uint32_t i = 0; i < w->partition_by.size; ++i) {
                if (i > 0) {
                    append(", ");
                }
                expr(w->partition_by[i], 0);
            }
        }
        if (!w->order_by.empty()) {
            sep();
            append("ORDER BY ");
            sort_list(w->order_by);
        }
        if (w->frame != nullptr) {
            sep();
            frame(w->frame);
        }
    }

    // Expressions.

    void function_call(const FunctionCall* n) {
        name_parts(n->name);
        out_.push_back('(');
        if (n->wildcard) {
            out_.push_back('*');
        } else {
            if (n->distinct) {
                append("DISTINCT ");
            }
            for (uint32_t i = 0; i < n->args.size; ++i) {
                if (i > 0) {
                    append(", ");
                }
                expr(n->args[i], 0);
            }
            if (!n->order_by.empty()) {
                if (!n->args.empty() || n->distinct) {
                    space();
                }
                append("ORDER BY ");
                sort_list(n->order_by);
            }
        }
        out_.push_back(')');
        if (n->filter != nullptr) {
            append(" FILTER (WHERE ");
            expr(n->filter, 0);
            out_.push_back(')');
        }
        if (n->window != nullptr) {
            append(" OVER (");
            window_contents(n->window);
            out_.push_back(')');
        } else if (n->has_window_ref) {
            append(" OVER ");
            name_part(n->window_ref);
        }
    }

    void listagg(const ListaggExpression* n) {
        append("LISTAGG(");
        if (n->distinct) {
            append("DISTINCT ");
        }
        expr(n->value, 0);
        if (n->separator != nullptr) {
            append(", ");
            expr(n->separator, 0);
            if (n->overflow != OverflowBehavior::kUnspecified) {
                append(" ON OVERFLOW ");
                if (n->overflow == OverflowBehavior::kError) {
                    append("ERROR");
                } else {
                    append("TRUNCATE");
                    if (!n->overflow_filler.empty()) {
                        space();
                        append(n->overflow_filler);
                    }
                    if (n->overflow_count == OverflowCount::kWith) {
                        append(" WITH COUNT");
                    } else if (n->overflow_count == OverflowCount::kWithout) {
                        append(" WITHOUT COUNT");
                    }
                }
            }
        }
        append(") WITHIN GROUP (ORDER BY ");
        sort_list(n->order_by);
        out_.push_back(')');
    }

    void like(const LikePredicate* n) {
        if (n->case_insensitive && !dialect_.ilike) {
            // No native ILIKE: rewrite as a case-folded LIKE.
            append("LOWER(");
            if (n->value != nullptr) {
                expr(n->value, 0);
            }
            out_.push_back(')');
            if (n->negated) {
                append(" NOT");
            }
            append(" LIKE LOWER(");
            expr(n->pattern, 0);
            out_.push_back(')');
        } else {
            if (n->value != nullptr) {
                expr(n->value, kPrecValue);
                space();
            }
            if (n->negated) {
                append("NOT ");
            }
            append(n->case_insensitive ? "ILIKE " : "LIKE ");
            expr(n->pattern, kPrecValue);
        }
        if (n->escape != nullptr) {
            append(" ESCAPE ");
            expr(n->escape, kPrecValue);
        }
    }

    void comparison(const ComparisonExpression* n) {
        if (n->op == ComparisonOp::kIsDistinctFrom) {
            if (n->left != nullptr) {
                expr(n->left, kPrecValue);
                space();
            }
            append("IS ");
            if (n->negated) {
                append("NOT ");
            }
            append("DISTINCT FROM ");
            expr(n->right, kPrecValue);
            return;
        }
        // A null left operand marks a partial predicate (simple CASE WHEN).
        if (n->left == nullptr) {
            append(comparison_op_symbol(n->op));
            space();
            expr(n->right, kPrecValue);
            return;
        }
        expr(n->left, kPrecValue);
        space();
        append(comparison_op_symbol(n->op));
        space();
        expr(n->right, kPrecValue);
    }

    void when_clauses(const AstList<WhenClause*>& clauses) {
        for (const WhenClause* clause : clauses) {
            append(" WHEN ");
            expr(clause->when, 0);
            append(" THEN ");
            expr(clause->result, 0);
        }
    }

    void visit_expr(const Expression* e) {
        switch (e->kind) {
            case NodeKind::kIdentifier: {
                const auto* n = e->as<Identifier>();
                identifier(n->name, n->quoted);
                break;
            }
            case NodeKind::kDereference: {
                const auto* n = e->as<DereferenceExpression>();
                expr(n->base, kPrecPostfix);
                out_.push_back('.');
                identifier(n->field, n->field_quoted);
                break;
            }
            case NodeKind::kNumberLiteral:
                append(e->as<NumberLiteral>()->value);
                break;
            case NodeKind::kStringLiteral: {
                const auto* n = e->as<StringLiteral>();
                append(n->value);
                if (n->escape != '\0') {
                    append(" UESCAPE '");
                    out_.push_back(n->escape);
                    out_.push_back('\'');
                }
                break;
            }
            case NodeKind::kBooleanLiteral:
                append(e->as<syntax::BooleanLiteral>()->value ? "TRUE" : "FALSE");
                break;
            case NodeKind::kNullLiteral:
                append("NULL");
                break;
            case NodeKind::kIntervalLiteral: {
                const auto* n = e->as<IntervalLiteral>();
                append("INTERVAL ");
                if (n->negative) {
                    out_.push_back('-');
                }
                append(n->value);
                space();
                append(n->from_unit);
                if (!n->to_unit.empty()) {
                    append(" TO ");
                    append(n->to_unit);
                }
                break;
            }
            case NodeKind::kTypedLiteral: {
                const auto* n = e->as<TypedLiteral>();
                append(n->type_name);
                space();
                append(n->value);
                break;
            }
            case NodeKind::kArithmeticBinary: {
                const auto* n = e->as<ArithmeticBinaryExpression>();
                const uint8_t prec = precedence(e);
                const char* sym = nullptr;
                switch (n->op) {
                    case ArithmeticOp::kAdd:
                        sym = "+";
                        break;
                    case ArithmeticOp::kSubtract:
                        sym = "-";
                        break;
                    case ArithmeticOp::kMultiply:
                        sym = "*";
                        break;
                    case ArithmeticOp::kDivide:
                        sym = "/";
                        break;
                    case ArithmeticOp::kModulus:
                        sym = "%";
                        break;
                    case ArithmeticOp::kConcatenate:
                        sym = "||";
                        break;
                }
                expr(n->left, prec);
                space();
                append(sym);
                space();
                expr(n->right, static_cast<uint8_t>(prec + 1));
                break;
            }
            case NodeKind::kArithmeticUnary: {
                const auto* n = e->as<ArithmeticUnaryExpression>();
                out_.push_back(n->negative ? '-' : '+');
                // '--9' would lex as a line comment; separate stacked signs.
                if (n->value->kind == NodeKind::kArithmeticUnary) {
                    space();
                }
                expr(n->value, kPrecUnary);
                break;
            }
            case NodeKind::kComparison:
                comparison(e->as<ComparisonExpression>());
                break;
            case NodeKind::kIsNull: {
                const auto* n = e->as<IsNullPredicate>();
                if (n->value != nullptr) {
                    expr(n->value, kPrecValue);
                    space();
                }
                append("IS ");
                if (n->negated) {
                    append("NOT ");
                }
                append("NULL");
                break;
            }
            case NodeKind::kBetween: {
                const auto* n = e->as<BetweenPredicate>();
                if (n->value != nullptr) {
                    expr(n->value, kPrecValue);
                    space();
                }
                if (n->negated) {
                    append("NOT ");
                }
                append("BETWEEN ");
                if (n->symmetry == BetweenSymmetry::kSymmetric) {
                    append("SYMMETRIC ");
                }
                expr(n->min, kPrecValue);
                append(" AND ");
                expr(n->max, kPrecValue);
                break;
            }
            case NodeKind::kInList: {
                const auto* n = e->as<InListExpression>();
                out_.push_back('(');
                for (uint32_t i = 0; i < n->items.size; ++i) {
                    if (i > 0) {
                        append(", ");
                    }
                    expr(n->items[i], 0);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kInPredicate: {
                const auto* n = e->as<InPredicate>();
                if (n->value != nullptr) {
                    expr(n->value, kPrecValue);
                    space();
                }
                if (n->negated) {
                    append("NOT ");
                }
                append("IN ");
                expr(n->value_list, kPrecPrimary);
                break;
            }
            case NodeKind::kLike:
                like(e->as<LikePredicate>());
                break;
            case NodeKind::kLogicalBinary: {
                const auto* n = e->as<LogicalBinaryExpression>();
                const uint8_t prec = precedence(e);
                expr(n->left, prec);
                space();
                append(n->op == LogicalOp::kAnd ? "AND" : "OR");
                space();
                expr(n->right, static_cast<uint8_t>(prec + 1));
                break;
            }
            case NodeKind::kNot: {
                append("NOT ");
                expr(e->as<NotExpression>()->value, kPrecNot);
                break;
            }
            case NodeKind::kExists: {
                append("EXISTS (");
                query(e->as<syntax::ExistsPredicate>()->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kSimpleCase: {
                const auto* n = e->as<SimpleCaseExpression>();
                append("CASE ");
                expr(n->operand, 0);
                when_clauses(n->when_clauses);
                if (n->else_result != nullptr) {
                    append(" ELSE ");
                    expr(n->else_result, 0);
                }
                append(" END");
                break;
            }
            case NodeKind::kSearchedCase: {
                const auto* n = e->as<SearchedCaseExpression>();
                append("CASE");
                when_clauses(n->when_clauses);
                if (n->else_result != nullptr) {
                    append(" ELSE ");
                    expr(n->else_result, 0);
                }
                append(" END");
                break;
            }
            case NodeKind::kCast: {
                const auto* n = e->as<Cast>();
                append((n->try_cast && dialect_.try_cast) ? "TRY_CAST(" : "CAST(");
                expr(n->expression, 0);
                append(" AS ");
                type(n->type);
                out_.push_back(')');
                break;
            }
            case NodeKind::kFunctionCall:
                function_call(e->as<FunctionCall>());
                break;
            case NodeKind::kSubscript: {
                const auto* n = e->as<SubscriptExpression>();
                expr(n->base, kPrecPostfix);
                out_.push_back('[');
                expr(n->index, 0);
                out_.push_back(']');
                break;
            }
            case NodeKind::kLambda: {
                const auto* n = e->as<LambdaExpression>();
                if (n->parameters.size == 1 && !n->parameters[0].quoted) {
                    name_part(n->parameters[0]);
                } else {
                    out_.push_back('(');
                    name_list(n->parameters);
                    out_.push_back(')');
                }
                append(" -> ");
                expr(n->body, 0);
                break;
            }
            case NodeKind::kRow: {
                const auto* n = e->as<Row>();
                append("ROW(");
                for (uint32_t i = 0; i < n->items.size; ++i) {
                    if (i > 0) {
                        append(", ");
                    }
                    expr(n->items[i], 0);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kArrayConstructor: {
                const auto* n = e->as<ArrayConstructor>();
                append("ARRAY[");
                for (uint32_t i = 0; i < n->items.size; ++i) {
                    if (i > 0) {
                        append(", ");
                    }
                    expr(n->items[i], 0);
                }
                out_.push_back(']');
                break;
            }
            case NodeKind::kSubqueryExpression: {
                out_.push_back('(');
                query(e->as<SubqueryExpression>()->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kParameter:
                out_.push_back('?');
                break;
            case NodeKind::kBooleanTest: {
                const auto* n = e->as<BooleanTestPredicate>();
                if (n->value != nullptr) {
                    expr(n->value, kPrecValue);
                    space();
                }
                append("IS ");
                if (n->negated) {
                    append("NOT ");
                }
                switch (n->test) {
                    case BooleanTestType::kTrue:
                        append("TRUE");
                        break;
                    case BooleanTestType::kFalse:
                        append("FALSE");
                        break;
                    case BooleanTestType::kUnknown:
                        append("UNKNOWN");
                        break;
                }
                break;
            }
            case NodeKind::kTrim: {
                const auto* n = e->as<TrimExpression>();
                append("TRIM(");
                if (n->chars != nullptr || n->spec != TrimSpec::kBoth) {
                    switch (n->spec) {
                        case TrimSpec::kBoth:
                            append("BOTH ");
                            break;
                        case TrimSpec::kLeading:
                            append("LEADING ");
                            break;
                        case TrimSpec::kTrailing:
                            append("TRAILING ");
                            break;
                    }
                    if (n->chars != nullptr) {
                        expr(n->chars, 0);
                        space();
                    }
                    append("FROM ");
                }
                expr(n->source, 0);
                out_.push_back(')');
                break;
            }
            case NodeKind::kSubstring: {
                const auto* n = e->as<SubstringExpression>();
                append("SUBSTRING(");
                expr(n->value, 0);
                append(" FROM ");
                expr(n->start, 0);
                if (n->length != nullptr) {
                    append(" FOR ");
                    expr(n->length, 0);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kPosition: {
                const auto* n = e->as<PositionExpression>();
                append("POSITION(");
                expr(n->needle, 0);
                append(" IN ");
                expr(n->haystack, 0);
                out_.push_back(')');
                break;
            }
            case NodeKind::kOverlay: {
                const auto* n = e->as<OverlayExpression>();
                append("OVERLAY(");
                expr(n->value, 0);
                append(" PLACING ");
                expr(n->replacement, 0);
                append(" FROM ");
                expr(n->start, 0);
                if (n->length != nullptr) {
                    append(" FOR ");
                    expr(n->length, 0);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kAtTimeZone: {
                const auto* n = e->as<AtTimeZone>();
                expr(n->value, kPrecPostfix);
                if (n->local) {
                    append(" AT LOCAL");
                } else {
                    append(" AT TIME ZONE ");
                    expr(n->zone, kPrecPrimary);
                }
                break;
            }
            case NodeKind::kQuantifiedComparison: {
                const auto* n = e->as<QuantifiedComparisonExpression>();
                if (n->value != nullptr) {
                    expr(n->value, kPrecValue);
                    space();
                }
                append(comparison_op_symbol(n->op));
                space();
                switch (n->quantifier) {
                    case Quantifier::kAny:
                        append("ANY");
                        break;
                    case Quantifier::kSome:
                        append("SOME");
                        break;
                    case Quantifier::kAll:
                        append("ALL");
                        break;
                }
                append(" (");
                query(n->subquery);
                out_.push_back(')');
                break;
            }
            case NodeKind::kMatchPredicate: {
                const auto* n = e->as<MatchPredicate>();
                if (n->value != nullptr) {
                    expr(n->value, kPrecValue);
                    space();
                }
                append("MATCH ");
                if (n->unique) {
                    append("UNIQUE ");
                }
                switch (n->match_type) {
                    case MatchType::kSimple:
                        append("SIMPLE ");
                        break;
                    case MatchType::kPartial:
                        append("PARTIAL ");
                        break;
                    case MatchType::kFull:
                        append("FULL ");
                        break;
                    case MatchType::kUnspecified:
                        break;
                }
                out_.push_back('(');
                query(n->subquery);
                out_.push_back(')');
                break;
            }
            case NodeKind::kGroupingOperation: {
                const auto* n = e->as<GroupingOperation>();
                append("GROUPING(");
                for (uint32_t i = 0; i < n->args.size; ++i) {
                    if (i > 0) {
                        append(", ");
                    }
                    expr(n->args[i], 0);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kGroupingAuto:
                append("AUTO");
                break;
            case NodeKind::kListagg:
                listagg(e->as<ListaggExpression>());
                break;
            default:
                append("<?>");
                break;
        }
    }

    // Relations.

    void relation(const Node* r) {
        switch (r->kind) {
            case NodeKind::kTable:
                name_parts(r->as<Table>()->name);
                break;
            case NodeKind::kAliasedRelation: {
                const auto* n = r->as<AliasedRelation>();
                // A join must be parenthesized before it can be aliased.
                const bool parens = n->relation->kind == NodeKind::kJoin;
                if (parens) {
                    out_.push_back('(');
                }
                relation(n->relation);
                if (parens) {
                    out_.push_back(')');
                }
                append(" AS ");
                name_part(n->alias);
                if (!n->column_aliases.empty()) {
                    append(" (");
                    name_list(n->column_aliases);
                    out_.push_back(')');
                }
                break;
            }
            case NodeKind::kJoin: {
                const auto* n = r->as<Join>();
                relation(n->left);
                if (n->natural) {
                    append(" NATURAL");
                }
                switch (n->join_type) {
                    case JoinType::kInner:
                        append(" INNER JOIN ");
                        break;
                    case JoinType::kLeft:
                        append(" LEFT JOIN ");
                        break;
                    case JoinType::kRight:
                        append(" RIGHT JOIN ");
                        break;
                    case JoinType::kFull:
                        append(" FULL JOIN ");
                        break;
                    case JoinType::kCross:
                        append(" CROSS JOIN ");
                        break;
                }
                // Joins chain to the left; a right-side join needs parens.
                if (n->right->kind == NodeKind::kJoin) {
                    out_.push_back('(');
                    relation(n->right);
                    out_.push_back(')');
                } else {
                    relation(n->right);
                }
                if (n->on != nullptr) {
                    append(" ON ");
                    expr(n->on, 0);
                } else if (!n->using_columns.empty()) {
                    append(" USING (");
                    name_list(n->using_columns);
                    out_.push_back(')');
                }
                break;
            }
            case NodeKind::kLateral: {
                append("LATERAL (");
                query(r->as<Lateral>()->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kTableSubquery: {
                out_.push_back('(');
                query(r->as<TableSubquery>()->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kTableSample: {
                const auto* n = r->as<TableSample>();
                relation(n->relation);
                append(n->sample_type == SampleType::kBernoulli ? " TABLESAMPLE BERNOULLI ("
                                                                : " TABLESAMPLE SYSTEM (");
                expr(n->percentage, 0);
                out_.push_back(')');
                break;
            }
            case NodeKind::kUnnest: {
                const auto* n = r->as<Unnest>();
                append("UNNEST(");
                for (uint32_t i = 0; i < n->expressions.size; ++i) {
                    if (i > 0) {
                        append(", ");
                    }
                    expr(n->expressions[i], 0);
                }
                out_.push_back(')');
                if (n->with_ordinality) {
                    append(" WITH ORDINALITY");
                }
                break;
            }
            case NodeKind::kValues: {
                // VALUES is not a relation primary; in FROM position it must
                // be enclosed in parens (parsed back as a table subquery).
                append("(VALUES ");
                const auto* n = r->as<Values>();
                for (uint32_t i = 0; i < n->rows.size; ++i) {
                    if (i > 0) {
                        append(", ");
                    }
                    expr(n->rows[i], 0);
                }
                out_.push_back(')');
                break;
            }
            default:
                append("<?>");
                break;
        }
    }

    // Queries and statements.

    // The body of a Query node, or one operand of a set operation.
    void query_body(const Node* body) {
        switch (body->kind) {
            case NodeKind::kQuery:
                // Only reachable via explicit parens in the source; keep them.
                out_.push_back('(');
                query(body->as<Query>());
                out_.push_back(')');
                break;
            case NodeKind::kTable:
                // The TABLE command form, not a relation.
                append("TABLE ");
                name_parts(body->as<Table>()->name);
                break;
            case NodeKind::kValues: {
                const auto* n = body->as<Values>();
                append("VALUES ");
                for (uint32_t i = 0; i < n->rows.size; ++i) {
                    if (i > 0) {
                        append(", ");
                    }
                    expr(n->rows[i], 0);
                }
                break;
            }
            default:
                visit(body);
                break;
        }
    }

    void set_operand(const Node* child, bool right_side, SetOp parent_op) {
        bool parens = false;
        if (child->kind == NodeKind::kSetOperation) {
            // INTERSECT binds tighter than UNION/EXCEPT; restore the parens
            // needed to keep the exact tree shape after a re-parse.
            const int child_prec = child->as<SetOperation>()->op == SetOp::kIntersect ? 2 : 1;
            const int parent_prec = parent_op == SetOp::kIntersect ? 2 : 1;
            parens = right_side ? (child_prec <= parent_prec) : (child_prec < parent_prec);
        }
        if (parens) {
            out_.push_back('(');
        }
        query_body(child);
        if (parens) {
            out_.push_back(')');
        }
    }

    void query(const Query* n) {
        if (n->with != nullptr) {
            with(n->with);
            space();
        }
        query_body(n->body);
        if (!n->order_by.empty()) {
            append(" ORDER BY ");
            sort_list(n->order_by);
        }
        if (n->offset != nullptr) {
            append(" OFFSET ");
            expr(n->offset, 0);
            append(" ROWS");
        }
        if (n->limit != nullptr) {
            append(" LIMIT ");
            expr(n->limit, 0);
        }
        if (n->fetch_first != nullptr || n->fetch_with_ties) {
            append(" FETCH FIRST ");
            if (n->fetch_first != nullptr) {
                expr(n->fetch_first, 0);
                space();
            }
            append("ROWS ");
            append(n->fetch_with_ties ? "WITH TIES" : "ONLY");
        }
    }

    void with(const With* n) {
        append("WITH ");
        if (n->recursive) {
            append("RECURSIVE ");
        }
        for (uint32_t i = 0; i < n->queries.size; ++i) {
            if (i > 0) {
                append(", ");
            }
            const WithQuery* wq = n->queries[i];
            name_part(wq->name);
            if (!wq->column_aliases.empty()) {
                append(" (");
                name_list(wq->column_aliases);
                out_.push_back(')');
            }
            append(" AS (");
            query(wq->query);
            out_.push_back(')');
        }
    }

    void query_specification(const QuerySpecification* n) {
        append("SELECT ");
        if (n->distinct) {
            append("DISTINCT ");
        }
        for (uint32_t i = 0; i < n->select_items.size; ++i) {
            if (i > 0) {
                append(", ");
            }
            visit(n->select_items[i]);
        }
        if (!n->from.empty()) {
            append(" FROM ");
            for (uint32_t i = 0; i < n->from.size; ++i) {
                if (i > 0) {
                    append(", ");
                }
                relation(n->from[i]);
            }
        }
        if (n->where != nullptr) {
            append(" WHERE ");
            expr(n->where, 0);
        }
        if (!n->group_by.empty()) {
            append(" GROUP BY ");
            if (n->group_by_distinct) {
                append("DISTINCT ");
            }
            for (uint32_t i = 0; i < n->group_by.size; ++i) {
                if (i > 0) {
                    append(", ");
                }
                expr(n->group_by[i], 0);
            }
        }
        if (n->having != nullptr) {
            append(" HAVING ");
            expr(n->having, 0);
        }
        if (!n->window_definitions.empty()) {
            append(" WINDOW ");
            for (uint32_t i = 0; i < n->window_definitions.size; ++i) {
                if (i > 0) {
                    append(", ");
                }
                const WindowDefinition* def = n->window_definitions[i];
                name_part(def->name);
                append(" AS (");
                window_contents(def->window);
                out_.push_back(')');
            }
        }
    }

    void visit(const Node* node) {
        switch (node->kind) {
            case NodeKind::kSingleColumn: {
                const auto* n = node->as<syntax::SingleColumn>();
                expr(n->expression, 0);
                if (n->has_alias) {
                    append(" AS ");
                    name_part(n->alias);
                }
                break;
            }
            case NodeKind::kAllColumns: {
                const auto* n = node->as<AllColumns>();
                if (n->target != nullptr) {
                    out_.push_back('(');
                    expr(n->target, 0);
                    append(").*");
                } else if (n->prefix.empty()) {
                    out_.push_back('*');
                } else {
                    name_parts(n->prefix);
                    append(".*");
                }
                if (!n->aliases.empty()) {
                    append(" AS (");
                    name_list(n->aliases);
                    out_.push_back(')');
                }
                break;
            }
            case NodeKind::kTable:
            case NodeKind::kAliasedRelation:
            case NodeKind::kJoin:
            case NodeKind::kLateral:
            case NodeKind::kTableSubquery:
            case NodeKind::kTableSample:
            case NodeKind::kUnnest:
                relation(node);
                break;
            case NodeKind::kValues: {
                const auto* n = node->as<Values>();
                append("VALUES ");
                for (uint32_t i = 0; i < n->rows.size; ++i) {
                    if (i > 0) {
                        append(", ");
                    }
                    expr(n->rows[i], 0);
                }
                break;
            }
            case NodeKind::kSortItem:
                sort_item(node->as<SortItem>());
                break;
            case NodeKind::kWindowDefinition: {
                const auto* n = node->as<WindowDefinition>();
                name_part(n->name);
                append(" AS (");
                window_contents(n->window);
                out_.push_back(')');
                break;
            }
            case NodeKind::kWithQuery: {
                const auto* n = node->as<WithQuery>();
                name_part(n->name);
                if (!n->column_aliases.empty()) {
                    append(" (");
                    name_list(n->column_aliases);
                    out_.push_back(')');
                }
                append(" AS (");
                query(n->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kWith:
                with(node->as<With>());
                break;
            case NodeKind::kQuerySpecification:
                query_specification(node->as<QuerySpecification>());
                break;
            case NodeKind::kSetOperation: {
                const auto* n = node->as<SetOperation>();
                set_operand(n->left, false, n->op);
                space();
                switch (n->op) {
                    case SetOp::kUnion:
                        append("UNION");
                        break;
                    case SetOp::kIntersect:
                        append("INTERSECT");
                        break;
                    case SetOp::kExcept:
                        append("EXCEPT");
                        break;
                }
                if (n->all) {
                    append(" ALL");
                }
                if (n->corresponding) {
                    append(" CORRESPONDING");
                    if (!n->corresponding_by.empty()) {
                        append(" BY (");
                        name_list(n->corresponding_by);
                        out_.push_back(')');
                    }
                }
                space();
                set_operand(n->right, true, n->op);
                break;
            }
            case NodeKind::kQuery:
                query(node->as<Query>());
                break;
            case NodeKind::kExplain: {
                const auto* n = node->as<Explain>();
                append("EXPLAIN ");
                if (n->analyze) {
                    append("ANALYZE ");
                }
                if (n->verbose) {
                    append("VERBOSE ");
                }
                if (!n->options.empty()) {
                    out_.push_back('(');
                    for (uint32_t i = 0; i < n->options.size; ++i) {
                        if (i > 0) {
                            append(", ");
                        }
                        name_part(n->options[i].name);
                        space();
                        name_part(n->options[i].value);
                    }
                    append(") ");
                }
                visit(n->statement);
                break;
            }
            case NodeKind::kWhenClause: {
                const auto* n = node->as<WhenClause>();
                append("WHEN ");
                expr(n->when, 0);
                append(" THEN ");
                expr(n->result, 0);
                break;
            }
            case NodeKind::kWindow:
                out_.push_back('(');
                window_contents(node->as<Window>());
                out_.push_back(')');
                break;
            case NodeKind::kWindowFrame:
                frame(node->as<WindowFrame>());
                break;
            case NodeKind::kTypeName:
                type(node->as<TypeName>());
                break;
            default:
                expr(static_cast<const Expression*>(node), 0);
                break;
        }
    }

    std::string out_;
    const dialect::Dialect& dialect_;
};

} // namespace

std::string print(const syntax::Node* node, const dialect::Dialect& dialect) {
    Printer printer(dialect);
    return printer.run(node);
}

} // namespace pl::prism::printer
