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

#include "cpp/pl/prism/plan/ast_to_plan.h"

#include <algorithm>
#include <utility>

namespace pl::prism::plan {

namespace syn = syntax;

void AstToPlan::fail(syn::SourceLocation loc, std::string message) {
    errors_.push_back(PlanError{loc, std::move(message)});
}

PlanResult AstToPlan::translate(const syn::Node* query) {
    PlanResult result;
    if (query->kind != syn::NodeKind::kQuery) {
        fail(query->location, "plan transform expects a Query node");
        result.errors = std::move(errors_);
        return result;
    }
    result.root = translate_query(query->as<syn::Query>());
    result.errors = std::move(errors_);
    if (!result.ok()) {
        result.root = nullptr;
    }
    return result;
}

PlanNode* AstToPlan::translate_query(const syn::Query* query) {
    if (query->with != nullptr) {
        fail(query->with->location, "WITH is not supported by the plan transform yet");
        return nullptr;
    }
    PlanNode* source = translate_query_body(query->body);
    if (source == nullptr) {
        return nullptr;
    }
    if (!query->order_by.empty()) {
        source = make<Sort>(query->location, query->order_by, source);
    }
    // LIMIT ALL leaves limit == nullptr and is a no-op on its own.
    if (query->offset != nullptr || query->limit != nullptr || query->fetch_first != nullptr ||
        query->fetch_with_ties) {
        syn::Expression* count = query->limit != nullptr ? query->limit : query->fetch_first;
        source = make<Limit>(query->location, count, query->offset, query->fetch_with_ties, source);
    }
    return source;
}

PlanNode* AstToPlan::translate_query_body(const syn::Node* body) {
    switch (body->kind) {
        case syn::NodeKind::kQuerySpecification:
            return translate_query_specification(body->as<syn::QuerySpecification>());
        case syn::NodeKind::kSetOperation: {
            const auto* n = body->as<syn::SetOperation>();
            if (n->corresponding) {
                fail(n->location, "set-operation CORRESPONDING is not supported yet");
                return nullptr;
            }
            PlanNode* left = translate_query_body(n->left);
            PlanNode* right = translate_query_body(n->right);
            if (left == nullptr || right == nullptr) {
                return nullptr;
            }
            return make<SetOperation>(n->location, n->op, n->all, left, right);
        }
        case syn::NodeKind::kValues: {
            const auto* n = body->as<syn::Values>();
            return make<Values>(n->location, n->rows);
        }
        case syn::NodeKind::kTable: {
            // The TABLE command form: a bare scan of the named table.
            const auto* n = body->as<syn::Table>();
            return make<TableScan>(
                n->location, n->name, syn::NamePart{}, false, syn::AstList<syn::NamePart>{});
        }
        case syn::NodeKind::kQuery:
            // A parenthesized query used as a set-operation operand.
            return translate_query(body->as<syn::Query>());
        default:
            fail(body->location, "unsupported query body in plan transform");
            return nullptr;
    }
}

PlanNode* AstToPlan::translate_query_specification(const syn::QuerySpecification* spec) {
    PlanNode* source = nullptr;
    if (!spec->from.empty()) {
        source = translate_from(spec->from);
        if (source == nullptr) {
            return nullptr;
        }
    }
    if (spec->where != nullptr) {
        source = make<Filter>(spec->where->location, spec->where, source);
    }
    if (!spec->group_by.empty()) {
        source = make<Aggregation>(spec->location, spec->group_by, spec->group_by_distinct, source);
    }
    if (spec->having != nullptr) {
        source = make<Filter>(spec->having->location, spec->having, source);
    }
    // Windowed functions of the select list. The calls also stay inside the
    // projections; a real engine would replace them with symbols.
    std::vector<syn::Expression*> window_functions;
    for (const syn::SelectItem* item : spec->select_items) {
        if (item->kind == syn::NodeKind::kSingleColumn) {
            collect_windowed(item->as<syn::SingleColumn>()->expression, window_functions);
        }
    }
    if (!window_functions.empty() || !spec->window_definitions.empty()) {
        source = make<Window>(
            spec->location, make_list(window_functions), spec->window_definitions, source);
    }
    std::vector<Projection> projections;
    projections.reserve(spec->select_items.size);
    for (const syn::SelectItem* item : spec->select_items) {
        if (item->kind == syn::NodeKind::kSingleColumn) {
            const auto* n = item->as<syn::SingleColumn>();
            projections.push_back(Projection{n->expression, n->alias, n->has_alias, false, {}});
            continue;
        }
        const auto* n = item->as<syn::AllColumns>();
        if (n->target != nullptr) {
            fail(n->location, "the (expr).* select form is not supported in plans yet");
            return nullptr;
        }
        projections.push_back(Projection{nullptr, {}, false, true, n->prefix});
    }
    return make<Project>(spec->location, make_list(projections), spec->distinct, source);
}

PlanNode* AstToPlan::translate_from(const syn::AstList<syn::Relation*>& from) {
    PlanNode* left = translate_relation(from[0]);
    for (uint32_t i = 1; i < from.size && left != nullptr; ++i) {
        PlanNode* right = translate_relation(from[i]);
        if (right == nullptr) {
            return nullptr;
        }
        // A multi-relation FROM list is a chain of cross joins.
        left = make<Join>(from[i]->location,
                          syn::JoinType::kCross,
                          false,
                          left,
                          right,
                          nullptr,
                          syn::AstList<syn::NamePart>{});
    }
    return left;
}

PlanNode* AstToPlan::translate_relation(const syn::Relation* relation) {
    switch (relation->kind) {
        case syn::NodeKind::kTable: {
            const auto* n = relation->as<syn::Table>();
            return make<TableScan>(
                n->location, n->name, syn::NamePart{}, false, syn::AstList<syn::NamePart>{});
        }
        case syn::NodeKind::kAliasedRelation: {
            const auto* n = relation->as<syn::AliasedRelation>();
            PlanNode* child = translate_relation(n->relation);
            if (child == nullptr) {
                return nullptr;
            }
            // Only a TableScan records its alias; for other relations the
            // alias is scoping information (name binding is out of scope).
            if (!n->column_aliases.empty() && child->kind != PlanKind::kTableScan) {
                fail(n->location, "column aliases on a non-table relation are not supported yet");
                return nullptr;
            }
            if (child->kind == PlanKind::kTableScan) {
                auto* scan = child->as<TableScan>();
                scan->alias = n->alias;
                scan->has_alias = true;
                scan->column_aliases = n->column_aliases;
            }
            return child;
        }
        case syn::NodeKind::kJoin: {
            const auto* n = relation->as<syn::Join>();
            PlanNode* left = translate_relation(n->left);
            PlanNode* right = translate_relation(n->right);
            if (left == nullptr || right == nullptr) {
                return nullptr;
            }
            return make<Join>(
                n->location, n->join_type, n->natural, left, right, n->on, n->using_columns);
        }
        case syn::NodeKind::kTableSubquery: {
            return translate_query(relation->as<syn::TableSubquery>()->query);
        }
        case syn::NodeKind::kUnnest: {
            const auto* n = relation->as<syn::Unnest>();
            return make<Unnest>(n->location, n->expressions, n->with_ordinality);
        }
        case syn::NodeKind::kLateral:
            fail(relation->location, "LATERAL is not supported by the plan transform yet");
            return nullptr;
        case syn::NodeKind::kTableSample:
            fail(relation->location, "TABLESAMPLE is not supported by the plan transform yet");
            return nullptr;
        default:
            fail(relation->location, "unsupported relation in plan transform");
            return nullptr;
    }
}

// Recursively collects FunctionCall nodes that carry an OVER clause.
// Subqueries are not descended into: their windowed calls belong to the
// subquery's own plan.
void AstToPlan::collect_windowed(syn::Expression* expr, std::vector<syn::Expression*>& out) {
    const auto each = [&out](syn::AstList<syn::Expression*> items) {
        for (syn::Expression* item : items) {
            collect_windowed(item, out);
        }
    };
    const auto each_sort_key = [&out](syn::AstList<syn::SortItem*> items) {
        for (syn::SortItem* item : items) {
            collect_windowed(item->sort_key, out);
        }
    };
    switch (expr->kind) {
        case syn::NodeKind::kDereference:
            collect_windowed(expr->as<syn::DereferenceExpression>()->base, out);
            break;
        case syn::NodeKind::kArithmeticBinary: {
            const auto* n = expr->as<syn::ArithmeticBinaryExpression>();
            collect_windowed(n->left, out);
            collect_windowed(n->right, out);
            break;
        }
        case syn::NodeKind::kArithmeticUnary:
            collect_windowed(expr->as<syn::ArithmeticUnaryExpression>()->value, out);
            break;
        case syn::NodeKind::kComparison: {
            const auto* n = expr->as<syn::ComparisonExpression>();
            if (n->left != nullptr) {
                collect_windowed(n->left, out);
            }
            collect_windowed(n->right, out);
            break;
        }
        case syn::NodeKind::kIsNull: {
            const auto* n = expr->as<syn::IsNullPredicate>();
            if (n->value != nullptr) {
                collect_windowed(n->value, out);
            }
            break;
        }
        case syn::NodeKind::kBetween: {
            const auto* n = expr->as<syn::BetweenPredicate>();
            if (n->value != nullptr) {
                collect_windowed(n->value, out);
            }
            collect_windowed(n->min, out);
            collect_windowed(n->max, out);
            break;
        }
        case syn::NodeKind::kInList:
            each(expr->as<syn::InListExpression>()->items);
            break;
        case syn::NodeKind::kInPredicate: {
            const auto* n = expr->as<syn::InPredicate>();
            if (n->value != nullptr) {
                collect_windowed(n->value, out);
            }
            if (n->value_list->kind == syn::NodeKind::kInList) {
                collect_windowed(n->value_list, out);
            }
            break;
        }
        case syn::NodeKind::kLike: {
            const auto* n = expr->as<syn::LikePredicate>();
            if (n->value != nullptr) {
                collect_windowed(n->value, out);
            }
            collect_windowed(n->pattern, out);
            if (n->escape != nullptr) {
                collect_windowed(n->escape, out);
            }
            break;
        }
        case syn::NodeKind::kLogicalBinary: {
            const auto* n = expr->as<syn::LogicalBinaryExpression>();
            collect_windowed(n->left, out);
            collect_windowed(n->right, out);
            break;
        }
        case syn::NodeKind::kNot:
            collect_windowed(expr->as<syn::NotExpression>()->value, out);
            break;
        case syn::NodeKind::kSimpleCase: {
            const auto* n = expr->as<syn::SimpleCaseExpression>();
            collect_windowed(n->operand, out);
            for (const syn::WhenClause* w : n->when_clauses) {
                collect_windowed(w->when, out);
                collect_windowed(w->result, out);
            }
            if (n->else_result != nullptr) {
                collect_windowed(n->else_result, out);
            }
            break;
        }
        case syn::NodeKind::kSearchedCase: {
            const auto* n = expr->as<syn::SearchedCaseExpression>();
            for (const syn::WhenClause* w : n->when_clauses) {
                collect_windowed(w->when, out);
                collect_windowed(w->result, out);
            }
            if (n->else_result != nullptr) {
                collect_windowed(n->else_result, out);
            }
            break;
        }
        case syn::NodeKind::kCast:
            collect_windowed(expr->as<syn::Cast>()->expression, out);
            break;
        case syn::NodeKind::kFunctionCall: {
            const auto* n = expr->as<syn::FunctionCall>();
            if (n->window != nullptr || n->has_window_ref) {
                out.push_back(expr);
            }
            each(n->args);
            if (n->filter != nullptr) {
                collect_windowed(n->filter, out);
            }
            each_sort_key(n->order_by);
            break;
        }
        case syn::NodeKind::kSubscript: {
            const auto* n = expr->as<syn::SubscriptExpression>();
            collect_windowed(n->base, out);
            collect_windowed(n->index, out);
            break;
        }
        case syn::NodeKind::kLambda:
            collect_windowed(expr->as<syn::LambdaExpression>()->body, out);
            break;
        case syn::NodeKind::kRow:
            each(expr->as<syn::Row>()->items);
            break;
        case syn::NodeKind::kArrayConstructor:
            each(expr->as<syn::ArrayConstructor>()->items);
            break;
        case syn::NodeKind::kTrim: {
            const auto* n = expr->as<syn::TrimExpression>();
            collect_windowed(n->source, out);
            if (n->chars != nullptr) {
                collect_windowed(n->chars, out);
            }
            break;
        }
        case syn::NodeKind::kSubstring: {
            const auto* n = expr->as<syn::SubstringExpression>();
            collect_windowed(n->value, out);
            collect_windowed(n->start, out);
            if (n->length != nullptr) {
                collect_windowed(n->length, out);
            }
            break;
        }
        case syn::NodeKind::kPosition: {
            const auto* n = expr->as<syn::PositionExpression>();
            collect_windowed(n->needle, out);
            collect_windowed(n->haystack, out);
            break;
        }
        case syn::NodeKind::kOverlay: {
            const auto* n = expr->as<syn::OverlayExpression>();
            collect_windowed(n->value, out);
            collect_windowed(n->replacement, out);
            collect_windowed(n->start, out);
            if (n->length != nullptr) {
                collect_windowed(n->length, out);
            }
            break;
        }
        case syn::NodeKind::kAtTimeZone: {
            const auto* n = expr->as<syn::AtTimeZone>();
            collect_windowed(n->value, out);
            if (n->zone != nullptr) {
                collect_windowed(n->zone, out);
            }
            break;
        }
        case syn::NodeKind::kBooleanTest: {
            const auto* n = expr->as<syn::BooleanTestPredicate>();
            if (n->value != nullptr) {
                collect_windowed(n->value, out);
            }
            break;
        }
        case syn::NodeKind::kQuantifiedComparison: {
            const auto* n = expr->as<syn::QuantifiedComparisonExpression>();
            if (n->value != nullptr) {
                collect_windowed(n->value, out);
            }
            break;
        }
        case syn::NodeKind::kMatchPredicate: {
            const auto* n = expr->as<syn::MatchPredicate>();
            if (n->value != nullptr) {
                collect_windowed(n->value, out);
            }
            break;
        }
        case syn::NodeKind::kGroupingOperation:
            each(expr->as<syn::GroupingOperation>()->args);
            break;
        case syn::NodeKind::kListagg: {
            const auto* n = expr->as<syn::ListaggExpression>();
            collect_windowed(n->value, out);
            if (n->separator != nullptr) {
                collect_windowed(n->separator, out);
            }
            each_sort_key(n->order_by);
            break;
        }
        // Leaves, and subqueries which form their own scope.
        default:
            break;
    }
}

} // namespace pl::prism::plan
