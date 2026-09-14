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

#include "cpp/pl/prism/plan/plan_dump.h"

#include "cpp/pl/prism/syntax/ast_dump.h"

namespace pl::prism::plan {

namespace {

class Dumper {
public:
    std::string run(const PlanNode* node) {
        visit(node);
        return std::move(out_);
    }

private:
    void append(std::string_view s) { out_.append(s); }
    void space() { out_.push_back(' '); }

    void name_parts(const syntax::AstList<syntax::NamePart>& parts) {
        for (uint32_t i = 0; i < parts.size; ++i) {
            if (i > 0) {
                out_.push_back('.');
            }
            append(parts[i].text);
        }
    }

    void name_list(const syntax::AstList<syntax::NamePart>& parts) {
        out_.push_back('(');
        for (uint32_t i = 0; i < parts.size; ++i) {
            if (i > 0) {
                space();
            }
            append(parts[i].text);
        }
        out_.push_back(')');
    }

    void expr(const syntax::Expression* e) { append(syntax::dump(e)); }

    // A nullable source is printed as '_' (SELECT/WHERE without FROM).
    void source(const PlanNode* node) {
        space();
        if (node == nullptr) {
            out_.push_back('_');
        } else {
            visit(node);
        }
    }

    void visit(const PlanNode* node) {
        switch (node->kind) {
            case PlanKind::kTableScan: {
                const auto* n = node->as<TableScan>();
                append("(scan ");
                name_parts(n->name);
                if (n->has_alias) {
                    append(" AS ");
                    append(n->alias.text);
                    if (!n->column_aliases.empty()) {
                        space();
                        name_list(n->column_aliases);
                    }
                }
                out_.push_back(')');
                break;
            }
            case PlanKind::kValues: {
                const auto* n = node->as<Values>();
                append("(values");
                for (syntax::Expression* row : n->rows) {
                    space();
                    expr(row);
                }
                out_.push_back(')');
                break;
            }
            case PlanKind::kUnnest: {
                const auto* n = node->as<Unnest>();
                append(n->with_ordinality ? "(unnest-ord" : "(unnest");
                for (syntax::Expression* e : n->expressions) {
                    space();
                    expr(e);
                }
                out_.push_back(')');
                break;
            }
            case PlanKind::kProject: {
                const auto* n = node->as<Project>();
                append("(project");
                if (n->distinct) {
                    append(" distinct");
                }
                for (const Projection& p : n->projections) {
                    space();
                    if (p.star) {
                        if (p.star_prefix.empty()) {
                            out_.push_back('*');
                        } else {
                            name_parts(p.star_prefix);
                            append(".*");
                        }
                    } else {
                        append("(col ");
                        expr(p.expression);
                        if (p.has_alias) {
                            space();
                            append(p.alias.text);
                        }
                        out_.push_back(')');
                    }
                }
                source(n->source);
                out_.push_back(')');
                break;
            }
            case PlanKind::kFilter: {
                const auto* n = node->as<Filter>();
                append("(filter ");
                expr(n->predicate);
                source(n->source);
                out_.push_back(')');
                break;
            }
            case PlanKind::kAggregation: {
                const auto* n = node->as<Aggregation>();
                append(n->distinct ? "(agg distinct (keys" : "(agg (keys");
                for (syntax::Expression* key : n->grouping_keys) {
                    space();
                    expr(key);
                }
                out_.push_back(')');
                source(n->source);
                out_.push_back(')');
                break;
            }
            case PlanKind::kWindow: {
                const auto* n = node->as<Window>();
                append("(window (fns");
                for (syntax::Expression* fn : n->functions) {
                    space();
                    expr(fn);
                }
                out_.push_back(')');
                if (!n->definitions.empty()) {
                    append(" (defs");
                    for (const syntax::WindowDefinition* def : n->definitions) {
                        space();
                        append(def->name.text);
                    }
                    out_.push_back(')');
                }
                source(n->source);
                out_.push_back(')');
                break;
            }
            case PlanKind::kSort: {
                const auto* n = node->as<Sort>();
                append("(sort (items");
                for (syntax::SortItem* item : n->items) {
                    space();
                    append(syntax::dump(item));
                }
                out_.push_back(')');
                source(n->source);
                out_.push_back(')');
                break;
            }
            case PlanKind::kLimit: {
                const auto* n = node->as<Limit>();
                append("(limit");
                if (n->count != nullptr) {
                    append(" (count ");
                    expr(n->count);
                    out_.push_back(')');
                }
                if (n->offset != nullptr) {
                    append(" (offset ");
                    expr(n->offset);
                    out_.push_back(')');
                }
                if (n->with_ties) {
                    append(" ties");
                }
                source(n->source);
                out_.push_back(')');
                break;
            }
            case PlanKind::kJoin: {
                const auto* n = node->as<Join>();
                append("(join ");
                if (n->natural) {
                    append("NATURAL ");
                }
                switch (n->join_type) {
                    case syntax::JoinType::kInner:
                        append("INNER");
                        break;
                    case syntax::JoinType::kLeft:
                        append("LEFT");
                        break;
                    case syntax::JoinType::kRight:
                        append("RIGHT");
                        break;
                    case syntax::JoinType::kFull:
                        append("FULL");
                        break;
                    case syntax::JoinType::kCross:
                        append("CROSS");
                        break;
                }
                space();
                visit(n->left);
                space();
                visit(n->right);
                if (n->on != nullptr) {
                    append(" (on ");
                    expr(n->on);
                    out_.push_back(')');
                }
                if (!n->using_columns.empty()) {
                    append(" (using ");
                    name_list(n->using_columns);
                    out_.push_back(')');
                }
                out_.push_back(')');
                break;
            }
            case PlanKind::kSetOperation: {
                const auto* n = node->as<SetOperation>();
                switch (n->op) {
                    case syntax::SetOp::kUnion:
                        append("(union");
                        break;
                    case syntax::SetOp::kIntersect:
                        append("(intersect");
                        break;
                    case syntax::SetOp::kExcept:
                        append("(except");
                        break;
                }
                if (n->all) {
                    append(" all");
                }
                space();
                visit(n->left);
                space();
                visit(n->right);
                out_.push_back(')');
                break;
            }
        }
    }

    std::string out_;
};

} // namespace

std::string dump(const PlanNode* node) {
    Dumper dumper;
    return dumper.run(node);
}

} // namespace pl::prism::plan
