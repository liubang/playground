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

#include "cpp/pl/prism/syntax/ast_dump.h"

namespace pl::prism::syntax {

namespace {

const char* arithmetic_op_symbol(ArithmeticOp op) {
    switch (op) {
        case ArithmeticOp::kAdd:
            return "+";
        case ArithmeticOp::kSubtract:
            return "-";
        case ArithmeticOp::kMultiply:
            return "*";
        case ArithmeticOp::kDivide:
            return "/";
        case ArithmeticOp::kModulus:
            return "%";
        case ArithmeticOp::kConcatenate:
            return "||";
    }
    return "?";
}

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
            return "isdistinct";
    }
    return "?";
}

const char* join_type_name(JoinType type) {
    switch (type) {
        case JoinType::kInner:
            return "INNER";
        case JoinType::kLeft:
            return "LEFT";
        case JoinType::kRight:
            return "RIGHT";
        case JoinType::kFull:
            return "FULL";
        case JoinType::kCross:
            return "CROSS";
    }
    return "?";
}

const char* set_op_name(SetOp op) {
    switch (op) {
        case SetOp::kUnion:
            return "union";
        case SetOp::kIntersect:
            return "intersect";
        case SetOp::kExcept:
            return "except";
    }
    return "?";
}

const char* frame_type_name(FrameType type) {
    switch (type) {
        case FrameType::kRows:
            return "ROWS";
        case FrameType::kRange:
            return "RANGE";
        case FrameType::kGroups:
            return "GROUPS";
    }
    return "?";
}

class Dumper {
public:
    std::string run(const Node* node) {
        visit(node);
        return std::move(out_);
    }

private:
    void append(std::string_view s) { out_.append(s); }
    void space() { out_.push_back(' '); }

    void name_parts(const AstList<NamePart>& parts) {
        for (uint32_t i = 0; i < parts.size; ++i) {
            if (i > 0) {
                out_.push_back('.');
            }
            append(parts[i].text);
        }
    }

    template <typename T> void each(const AstList<T>& items, void (Dumper::*fn)(T)) {
        for (T item : items) {
            space();
            (this->*fn)(item);
        }
    }

    void type(const TypeName* t) {
        append(t->name);
        if (!t->num_args.empty()) {
            out_.push_back('(');
            for (uint32_t i = 0; i < t->num_args.size; ++i) {
                if (i > 0) {
                    out_.push_back(',');
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
                    append(t->field_names[i].text);
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

    void frame_bound(const FrameBound& bound) {
        switch (bound.type) {
            case FrameBoundType::kUnboundedPreceding:
                append("(unbounded preceding)");
                break;
            case FrameBoundType::kPreceding:
                append("(preceding ");
                visit(bound.value);
                out_.push_back(')');
                break;
            case FrameBoundType::kCurrentRow:
                append("(current row)");
                break;
            case FrameBoundType::kFollowing:
                append("(following ");
                visit(bound.value);
                out_.push_back(')');
                break;
            case FrameBoundType::kUnboundedFollowing:
                append("(unbounded following)");
                break;
        }
    }

    void sort_item(const SortItem* item) {
        append("(item ");
        visit(item->sort_key);
        if (item->ordering == Ordering::kAsc) {
            append(" asc");
        } else if (item->ordering == Ordering::kDesc) {
            append(" desc");
        }
        if (item->null_ordering == NullOrdering::kFirst) {
            append(" nulls first");
        } else if (item->null_ordering == NullOrdering::kLast) {
            append(" nulls last");
        }
        out_.push_back(')');
    }

    void visit(const Node* node) {
        switch (node->kind) {
            case NodeKind::kIdentifier:
                append(node->as<Identifier>()->name);
                break;
            case NodeKind::kDereference: {
                const auto* n = node->as<DereferenceExpression>();
                visit(n->base);
                out_.push_back('.');
                append(n->field);
                break;
            }
            case NodeKind::kNumberLiteral:
                append(node->as<NumberLiteral>()->value);
                break;
            case NodeKind::kStringLiteral:
                append(node->as<StringLiteral>()->value);
                break;
            case NodeKind::kBooleanLiteral:
                append(node->as<BooleanLiteral>()->value ? "true" : "false");
                break;
            case NodeKind::kNullLiteral:
                append("null");
                break;
            case NodeKind::kIntervalLiteral: {
                const auto* n = node->as<IntervalLiteral>();
                append("(interval ");
                if (n->negative) {
                    append("-");
                }
                append(n->value);
                space();
                append(n->from_unit);
                if (!n->to_unit.empty()) {
                    append(" TO ");
                    append(n->to_unit);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kTypedLiteral: {
                const auto* n = node->as<TypedLiteral>();
                append(n->type_name);
                space();
                append(n->value);
                break;
            }
            case NodeKind::kArithmeticBinary: {
                const auto* n = node->as<ArithmeticBinaryExpression>();
                out_.push_back('(');
                append(arithmetic_op_symbol(n->op));
                space();
                visit(n->left);
                space();
                visit(n->right);
                out_.push_back(')');
                break;
            }
            case NodeKind::kArithmeticUnary: {
                const auto* n = node->as<ArithmeticUnaryExpression>();
                append(n->negative ? "(neg " : "(pos ");
                visit(n->value);
                out_.push_back(')');
                break;
            }
            case NodeKind::kComparison: {
                const auto* n = node->as<ComparisonExpression>();
                out_.push_back('(');
                if (n->op == ComparisonOp::kIsDistinctFrom && n->negated) {
                    append("isnotdistinct");
                } else {
                    append(comparison_op_symbol(n->op));
                }
                space();
                visit(n->left);
                space();
                visit(n->right);
                out_.push_back(')');
                break;
            }
            case NodeKind::kIsNull: {
                const auto* n = node->as<IsNullPredicate>();
                append(n->negated ? "(notnull " : "(isnull ");
                visit(n->value);
                out_.push_back(')');
                break;
            }
            case NodeKind::kBetween: {
                const auto* n = node->as<BetweenPredicate>();
                append(n->negated ? "(notbetween " : "(between ");
                visit(n->value);
                space();
                visit(n->min);
                space();
                visit(n->max);
                out_.push_back(')');
                break;
            }
            case NodeKind::kInList: {
                const auto* n = node->as<InListExpression>();
                append("(list");
                each<Expression*>(n->items, &Dumper::visit_expr);
                out_.push_back(')');
                break;
            }
            case NodeKind::kInPredicate: {
                const auto* n = node->as<InPredicate>();
                append(n->negated ? "(notin " : "(in ");
                visit(n->value);
                space();
                visit(n->value_list);
                out_.push_back(')');
                break;
            }
            case NodeKind::kLike: {
                const auto* n = node->as<LikePredicate>();
                out_.push_back('(');
                if (n->negated) {
                    append("not");
                }
                append(n->case_insensitive ? "ilike " : "like ");
                visit(n->value);
                space();
                visit(n->pattern);
                if (n->escape != nullptr) {
                    append(" (esc ");
                    visit(n->escape);
                    out_.push_back(')');
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kLogicalBinary: {
                const auto* n = node->as<LogicalBinaryExpression>();
                append(n->op == LogicalOp::kAnd ? "(and " : "(or ");
                visit(n->left);
                space();
                visit(n->right);
                out_.push_back(')');
                break;
            }
            case NodeKind::kNot: {
                append("(not ");
                visit(node->as<NotExpression>()->value);
                out_.push_back(')');
                break;
            }
            case NodeKind::kExists: {
                append("(exists ");
                visit(node->as<ExistsPredicate>()->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kWhenClause: {
                const auto* n = node->as<WhenClause>();
                append("(when ");
                visit(n->when);
                space();
                visit(n->result);
                out_.push_back(')');
                break;
            }
            case NodeKind::kSimpleCase: {
                const auto* n = node->as<SimpleCaseExpression>();
                append("(case ");
                visit(n->operand);
                each<WhenClause*>(n->when_clauses, &Dumper::visit_when);
                if (n->else_result != nullptr) {
                    append(" (else ");
                    visit(n->else_result);
                    out_.push_back(')');
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kSearchedCase: {
                const auto* n = node->as<SearchedCaseExpression>();
                append("(case");
                each<WhenClause*>(n->when_clauses, &Dumper::visit_when);
                if (n->else_result != nullptr) {
                    append(" (else ");
                    visit(n->else_result);
                    out_.push_back(')');
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kCast: {
                const auto* n = node->as<Cast>();
                append(n->try_cast ? "(trycast " : "(cast ");
                visit(n->expression);
                space();
                type(n->type);
                out_.push_back(')');
                break;
            }
            case NodeKind::kFunctionCall: {
                const auto* n = node->as<FunctionCall>();
                append("(call ");
                name_parts(n->name);
                if (n->wildcard) {
                    append(" *");
                }
                if (n->distinct) {
                    append(" distinct");
                }
                each<Expression*>(n->args, &Dumper::visit_expr);
                if (!n->order_by.empty()) {
                    append(" (ord");
                    each<SortItem*>(n->order_by, &Dumper::visit_sort_item);
                    out_.push_back(')');
                }
                if (n->filter != nullptr) {
                    append(" (filter ");
                    visit(n->filter);
                    out_.push_back(')');
                }
                if (n->window != nullptr) {
                    space();
                    visit(n->window);
                }
                if (n->has_window_ref) {
                    append(" (over ");
                    append(n->window_ref.text);
                    out_.push_back(')');
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kWindow: {
                const auto* n = node->as<Window>();
                append("(over");
                if (n->has_existing_window) {
                    space();
                    append(n->existing_window.text);
                }
                if (!n->partition_by.empty()) {
                    append(" (part");
                    each<Expression*>(n->partition_by, &Dumper::visit_expr);
                    out_.push_back(')');
                }
                if (!n->order_by.empty()) {
                    append(" (order");
                    each<SortItem*>(n->order_by, &Dumper::visit_sort_item);
                    out_.push_back(')');
                }
                if (n->frame != nullptr) {
                    space();
                    visit(n->frame);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kWindowFrame: {
                const auto* n = node->as<WindowFrame>();
                append("(frame ");
                append(frame_type_name(n->frame_type));
                append(" (start ");
                frame_bound(n->start);
                out_.push_back(')');
                if (n->has_end) {
                    append(" (end ");
                    frame_bound(n->end);
                    out_.push_back(')');
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kSubscript: {
                const auto* n = node->as<SubscriptExpression>();
                append("(subscript ");
                visit(n->base);
                space();
                visit(n->index);
                out_.push_back(')');
                break;
            }
            case NodeKind::kLambda: {
                const auto* n = node->as<LambdaExpression>();
                append("(lambda (");
                for (uint32_t i = 0; i < n->parameters.size; ++i) {
                    if (i > 0) {
                        space();
                    }
                    append(n->parameters[i].text);
                }
                append(") ");
                visit(n->body);
                out_.push_back(')');
                break;
            }
            case NodeKind::kRow: {
                const auto* n = node->as<Row>();
                append("(row");
                each<Expression*>(n->items, &Dumper::visit_expr);
                out_.push_back(')');
                break;
            }
            case NodeKind::kArrayConstructor: {
                const auto* n = node->as<ArrayConstructor>();
                append("(array");
                each<Expression*>(n->items, &Dumper::visit_expr);
                out_.push_back(')');
                break;
            }
            case NodeKind::kSubqueryExpression: {
                append("(subquery ");
                visit(node->as<SubqueryExpression>()->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kTypeName:
                type(node->as<TypeName>());
                break;
            case NodeKind::kParameter:
                out_.push_back('?');
                break;
            case NodeKind::kBooleanTest: {
                const auto* n = node->as<BooleanTestPredicate>();
                out_.push_back('(');
                if (n->negated) {
                    append("not");
                }
                switch (n->test) {
                    case BooleanTestType::kTrue:
                        append("istrue ");
                        break;
                    case BooleanTestType::kFalse:
                        append("isfalse ");
                        break;
                    case BooleanTestType::kUnknown:
                        append("isunknown ");
                        break;
                }
                visit(n->value);
                out_.push_back(')');
                break;
            }
            case NodeKind::kTrim: {
                const auto* n = node->as<TrimExpression>();
                append("(trim ");
                switch (n->spec) {
                    case TrimSpec::kBoth:
                        append("both ");
                        break;
                    case TrimSpec::kLeading:
                        append("leading ");
                        break;
                    case TrimSpec::kTrailing:
                        append("trailing ");
                        break;
                }
                if (n->chars != nullptr) {
                    visit(n->chars);
                    space();
                }
                visit(n->source);
                out_.push_back(')');
                break;
            }
            case NodeKind::kSubstring: {
                const auto* n = node->as<SubstringExpression>();
                append("(substr ");
                visit(n->value);
                space();
                visit(n->start);
                if (n->length != nullptr) {
                    space();
                    visit(n->length);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kPosition: {
                const auto* n = node->as<PositionExpression>();
                append("(position ");
                visit(n->needle);
                space();
                visit(n->haystack);
                out_.push_back(')');
                break;
            }
            case NodeKind::kOverlay: {
                const auto* n = node->as<OverlayExpression>();
                append("(overlay ");
                visit(n->value);
                space();
                visit(n->replacement);
                space();
                visit(n->start);
                if (n->length != nullptr) {
                    space();
                    visit(n->length);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kAtTimeZone: {
                const auto* n = node->as<AtTimeZone>();
                append("(attz ");
                visit(n->value);
                space();
                visit(n->zone);
                out_.push_back(')');
                break;
            }
            case NodeKind::kGroupingOperation: {
                const auto* n = node->as<GroupingOperation>();
                append("(grouping");
                each<Expression*>(n->args, &Dumper::visit_expr);
                out_.push_back(')');
                break;
            }
            case NodeKind::kQuantifiedComparison: {
                const auto* n = node->as<QuantifiedComparisonExpression>();
                append("(qcmp ");
                append(comparison_op_symbol(n->op));
                space();
                switch (n->quantifier) {
                    case Quantifier::kAny:
                        append("any ");
                        break;
                    case Quantifier::kSome:
                        append("some ");
                        break;
                    case Quantifier::kAll:
                        append("all ");
                        break;
                }
                visit(n->value);
                space();
                visit(n->subquery);
                out_.push_back(')');
                break;
            }
            case NodeKind::kSingleColumn: {
                const auto* n = node->as<SingleColumn>();
                append("(col ");
                visit(n->expression);
                if (n->has_alias) {
                    space();
                    append(n->alias.text);
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kAllColumns: {
                const auto* n = node->as<AllColumns>();
                if (n->prefix.empty()) {
                    out_.push_back('*');
                } else {
                    name_parts(n->prefix);
                    append(".*");
                }
                break;
            }
            case NodeKind::kTable: {
                append("(table ");
                name_parts(node->as<Table>()->name);
                out_.push_back(')');
                break;
            }
            case NodeKind::kAliasedRelation: {
                const auto* n = node->as<AliasedRelation>();
                append("(alias ");
                visit(n->relation);
                space();
                append(n->alias.text);
                if (!n->column_aliases.empty()) {
                    append(" (");
                    for (uint32_t i = 0; i < n->column_aliases.size; ++i) {
                        if (i > 0) {
                            space();
                        }
                        append(n->column_aliases[i].text);
                    }
                    out_.push_back(')');
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kJoin: {
                const auto* n = node->as<Join>();
                append("(join ");
                if (n->natural) {
                    append("NATURAL ");
                }
                append(join_type_name(n->join_type));
                space();
                visit(n->left);
                space();
                visit(n->right);
                if (n->on != nullptr) {
                    append(" (on ");
                    visit(n->on);
                    out_.push_back(')');
                }
                if (!n->using_columns.empty()) {
                    append(" (using (");
                    for (uint32_t i = 0; i < n->using_columns.size; ++i) {
                        if (i > 0) {
                            space();
                        }
                        append(n->using_columns[i].text);
                    }
                    append("))");
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kLateral: {
                append("(lateral ");
                visit(node->as<Lateral>()->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kTableSubquery: {
                append("(tsub ");
                visit(node->as<TableSubquery>()->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kTableSample: {
                const auto* n = node->as<TableSample>();
                append(n->sample_type == SampleType::kBernoulli ? "(sample BERNOULLI "
                                                                : "(sample SYSTEM ");
                visit(n->relation);
                space();
                visit(n->percentage);
                out_.push_back(')');
                break;
            }
            case NodeKind::kUnnest: {
                const auto* n = node->as<Unnest>();
                append(n->with_ordinality ? "(unnest-ord" : "(unnest");
                each<Expression*>(n->expressions, &Dumper::visit_expr);
                out_.push_back(')');
                break;
            }
            case NodeKind::kValues: {
                const auto* n = node->as<Values>();
                append("(values");
                each<Expression*>(n->rows, &Dumper::visit_expr);
                out_.push_back(')');
                break;
            }
            case NodeKind::kSortItem:
                sort_item(node->as<SortItem>());
                break;
            case NodeKind::kWindowDefinition: {
                const auto* n = node->as<WindowDefinition>();
                append("(wdef ");
                append(n->name.text);
                space();
                visit(n->window);
                out_.push_back(')');
                break;
            }
            case NodeKind::kWithQuery: {
                const auto* n = node->as<WithQuery>();
                append("(wq ");
                append(n->name.text);
                if (!n->column_aliases.empty()) {
                    append(" (");
                    for (uint32_t i = 0; i < n->column_aliases.size; ++i) {
                        if (i > 0) {
                            space();
                        }
                        append(n->column_aliases[i].text);
                    }
                    out_.push_back(')');
                }
                space();
                visit(n->query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kWith: {
                const auto* n = node->as<With>();
                append(n->recursive ? "(with recursive" : "(with");
                each<WithQuery*>(n->queries, &Dumper::visit_with_query);
                out_.push_back(')');
                break;
            }
            case NodeKind::kQuerySpecification: {
                const auto* n = node->as<QuerySpecification>();
                append("(spec (select");
                if (n->distinct) {
                    append(" distinct");
                }
                each<SelectItem*>(n->select_items, &Dumper::visit_select_item);
                out_.push_back(')');
                if (!n->from.empty()) {
                    append(" (from");
                    each<Relation*>(n->from, &Dumper::visit_relation);
                    out_.push_back(')');
                }
                if (n->where != nullptr) {
                    append(" (where ");
                    visit(n->where);
                    out_.push_back(')');
                }
                if (!n->group_by.empty()) {
                    append(" (group");
                    each<Expression*>(n->group_by, &Dumper::visit_expr);
                    out_.push_back(')');
                }
                if (n->having != nullptr) {
                    append(" (having ");
                    visit(n->having);
                    out_.push_back(')');
                }
                if (!n->window_definitions.empty()) {
                    append(" (win");
                    each<WindowDefinition*>(n->window_definitions, &Dumper::visit_window_def);
                    out_.push_back(')');
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kSetOperation: {
                const auto* n = node->as<SetOperation>();
                out_.push_back('(');
                append(set_op_name(n->op));
                if (n->all) {
                    append(" all");
                }
                if (n->corresponding) {
                    append(" corresponding");
                    if (!n->corresponding_by.empty()) {
                        append(" (");
                        for (uint32_t i = 0; i < n->corresponding_by.size; ++i) {
                            if (i > 0) {
                                space();
                            }
                            append(n->corresponding_by[i].text);
                        }
                        out_.push_back(')');
                    }
                }
                space();
                visit(n->left);
                space();
                visit(n->right);
                out_.push_back(')');
                break;
            }
            case NodeKind::kQuery: {
                const auto* n = node->as<Query>();
                append("(query");
                if (n->with != nullptr) {
                    space();
                    visit(n->with);
                }
                space();
                visit(n->body);
                if (!n->order_by.empty()) {
                    append(" (order");
                    each<SortItem*>(n->order_by, &Dumper::visit_sort_item);
                    out_.push_back(')');
                }
                if (n->offset != nullptr) {
                    append(" (offset ");
                    visit(n->offset);
                    out_.push_back(')');
                }
                if (n->limit != nullptr) {
                    append(" (limit ");
                    visit(n->limit);
                    out_.push_back(')');
                }
                if (n->fetch_first != nullptr || n->fetch_with_ties) {
                    append(" (fetch");
                    if (n->fetch_first != nullptr) {
                        space();
                        visit(n->fetch_first);
                    }
                    if (n->fetch_with_ties) {
                        append(" ties");
                    }
                    out_.push_back(')');
                }
                out_.push_back(')');
                break;
            }
            case NodeKind::kExplain: {
                const auto* n = node->as<Explain>();
                append("(explain");
                if (n->analyze) {
                    append(" analyze");
                }
                if (n->verbose) {
                    append(" verbose");
                }
                for (const ExplainOption& opt : n->options) {
                    append(" (opt ");
                    append(opt.name.text);
                    space();
                    append(opt.value.text);
                    out_.push_back(')');
                }
                space();
                visit(n->statement);
                out_.push_back(')');
                break;
            }
        }
    }

    // Typed wrappers for use with each<>.
    void visit_expr(Expression* n) { visit(n); }
    void visit_when(WhenClause* n) { visit(n); }
    void visit_sort_item(SortItem* n) { sort_item(n); }
    void visit_select_item(SelectItem* n) { visit(n); }
    void visit_relation(Relation* n) { visit(n); }
    void visit_with_query(WithQuery* n) { visit(n); }
    void visit_window_def(WindowDefinition* n) { visit(n); }

    std::string out_;
};

} // namespace

std::string dump(const Node* node) {
    Dumper dumper;
    return dumper.run(node);
}

} // namespace pl::prism::syntax
