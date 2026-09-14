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
// Created: 2026/09/14 10:16

// Query structure: SELECT core, WITH, set operations, VALUES, EXPLAIN and
// sort items. Joins and table primaries live in parser_relation.cpp.

#include "cpp/pl/prism/syntax/parser.h"

namespace pl::prism::syntax {

Query* Parser::parse_query() {
    const SourceLocation loc = loc_of(cur());
    With* with = nullptr;
    if (at(TokenType::kKwWith)) {
        with = parse_with();
    }
    Node* body = parse_set_operation();

    std::vector<SortItem*> order_by;
    if (match(TokenType::kKwOrder)) {
        expect(TokenType::kKwBy, "BY after ORDER");
        order_by = parse_sort_list();
    }
    Expression* offset = nullptr;
    if (match_soft("offset")) {
        offset = parse_expr();
        match_soft("row");
        match_soft("rows");
    }
    Expression* limit = nullptr;
    Expression* fetch_first = nullptr;
    bool fetch_with_ties = false;
    if (match(TokenType::kKwLimit)) {
        if (!match_soft("all")) {
            limit = parse_expr();
        }
    } else if (at_soft("fetch")) {
        advance();
        if (!match_soft("first") && !match_soft("next")) {
            fail(cur(), "expected FIRST or NEXT after FETCH");
        }
        if (at(TokenType::kNumber)) {
            fetch_first = parse_expr();
        }
        if (!match_soft("row") && !match_soft("rows")) {
            fail(cur(), "expected ROW or ROWS in FETCH clause");
        }
        if (match_soft("only")) {
            fetch_with_ties = false;
        } else if (match(TokenType::kKwWith)) {
            if (!match_soft("ties")) {
                fail(cur(), "expected TIES after WITH");
            }
            fetch_with_ties = true;
        } else {
            fail(cur(), "expected ONLY or WITH TIES in FETCH clause");
        }
    }
    return make<Query>(
        loc, with, body, make_list(order_by), offset, limit, fetch_first, fetch_with_ties);
}

With* Parser::parse_with() {
    const SourceLocation loc = loc_of(advance()); // WITH
    const bool recursive = match(TokenType::kKwRecursive);
    std::vector<WithQuery*> queries;
    do {
        const SourceLocation query_loc = loc_of(cur());
        const NamePart name = parse_name_part();
        std::vector<NamePart> columns;
        if (match(TokenType::kLParen)) {
            if (!at(TokenType::kRParen)) {
                columns.push_back(parse_name_part());
                while (match(TokenType::kComma)) {
                    columns.push_back(parse_name_part());
                }
            }
            expect(TokenType::kRParen, "')' after column aliases");
        }
        expect(TokenType::kKwAs, "AS in WITH query");
        expect(TokenType::kLParen, "'(' after AS in WITH query");
        Query* query = parse_query();
        expect(TokenType::kRParen, "')' after WITH query");
        queries.push_back(make<WithQuery>(query_loc, name, make_list(columns), query));
    } while (match(TokenType::kComma));
    return make<With>(loc, recursive, make_list(queries));
}

Node* Parser::parse_explain() {
    const SourceLocation loc = loc_of(advance()); // EXPLAIN
    const bool analyze = match(TokenType::kKwAnalyze);
    const bool verbose = match_soft("verbose");
    std::vector<ExplainOption> options;
    if (match(TokenType::kLParen)) {
        do {
            const NamePart name = parse_name_part();
            const NamePart value = parse_name_part();
            options.push_back(ExplainOption{name, value});
        } while (match(TokenType::kComma));
        expect(TokenType::kRParen, "')' after EXPLAIN options");
    }
    // P0 scope: the explained statement is a query; DDL statements land in P1.
    Node* statement = parse_query();
    return make<Explain>(loc, analyze, verbose, make_list(options), statement);
}

Node* Parser::parse_set_operation() {
    Node* left = parse_intersect();
    while (at(TokenType::kKwUnion) || at(TokenType::kKwExcept)) {
        const SetOp op = at(TokenType::kKwUnion) ? SetOp::kUnion : SetOp::kExcept;
        const SourceLocation loc = loc_of(advance());
        const bool all = match_soft("all");
        if (!all) {
            match(TokenType::kKwDistinct);
        }
        bool corresponding = false;
        std::vector<NamePart> corresponding_by;
        parse_corresponding(corresponding, corresponding_by);
        left = make<SetOperation>(
            loc, op, left, parse_intersect(), all, corresponding, make_list(corresponding_by));
    }
    return left;
}

void Parser::parse_corresponding(bool& corresponding, std::vector<NamePart>& corresponding_by) {
    if (!at_soft("corresponding")) {
        return;
    }
    advance();
    corresponding = true;
    if (match(TokenType::kKwBy)) {
        expect(TokenType::kLParen, "'(' after BY");
        corresponding_by.push_back(parse_name_part());
        while (match(TokenType::kComma)) {
            corresponding_by.push_back(parse_name_part());
        }
        expect(TokenType::kRParen, "')' after BY");
    }
}

Node* Parser::parse_intersect() {
    Node* left = parse_query_term();
    while (at(TokenType::kKwIntersect)) {
        const SourceLocation loc = loc_of(advance());
        const bool all = match_soft("all");
        if (!all) {
            match(TokenType::kKwDistinct);
        }
        bool corresponding = false;
        std::vector<NamePart> corresponding_by;
        parse_corresponding(corresponding, corresponding_by);
        left = make<SetOperation>(loc,
                                  SetOp::kIntersect,
                                  left,
                                  parse_query_term(),
                                  all,
                                  corresponding,
                                  make_list(corresponding_by));
    }
    return left;
}

Node* Parser::parse_query_term() {
    if (at(TokenType::kLParen) && is_query_start(1)) {
        advance();
        Node* query = parse_query();
        expect(TokenType::kRParen, "')' after parenthesized query");
        return query;
    }
    if (at(TokenType::kKwSelect)) {
        return parse_query_specification();
    }
    if (at(TokenType::kKwValues)) {
        return parse_values();
    }
    if (at(TokenType::kKwTable)) {
        const SourceLocation loc = loc_of(advance());
        return make<Table>(loc, parse_qualified_name());
    }
    fail(cur(), "expected SELECT, VALUES or TABLE");
}

Node* Parser::parse_query_specification() {
    const SourceLocation loc = loc_of(advance()); // SELECT
    bool distinct = false;
    // ALL is a soft keyword: a set quantifier only when a select item
    // follows, otherwise an identifier (SELECT ALL, SOME, ANY FROM t).
    if (at_soft("all") && peek(1).type != TokenType::kComma && peek(1).type != TokenType::kKwFrom &&
        peek(1).type != TokenType::kEof && peek(1).type != TokenType::kSemicolon) {
        advance();
    } else {
        distinct = match(TokenType::kKwDistinct);
    }

    std::vector<SelectItem*> items;
    items.push_back(parse_select_item());
    while (match(TokenType::kComma)) {
        items.push_back(parse_select_item());
    }

    std::vector<Relation*> from;
    if (match(TokenType::kKwFrom)) {
        from.push_back(parse_relation());
        while (match(TokenType::kComma)) {
            from.push_back(parse_relation());
        }
    }
    Expression* where = nullptr;
    if (match(TokenType::kKwWhere)) {
        where = parse_expr();
    }
    std::vector<Expression*> group_by;
    bool group_by_distinct = false;
    if (match(TokenType::kKwGroup)) {
        expect(TokenType::kKwBy, "BY after GROUP");
        if (match(TokenType::kKwDistinct)) {
            group_by_distinct = true;
        } else if (at_soft("all")) {
            // ALL quantifies the grouping sets only when a grouping element
            // follows; otherwise it is an ordinary column name.
            const Token& next = peek(1);
            const std::string_view text =
                next.type == TokenType::kIdentifier ? next.text(source_) : std::string_view{};
            const bool element_follows =
                next.type != TokenType::kComma && next.type != TokenType::kEof &&
                next.type != TokenType::kSemicolon && next.type != TokenType::kRParen &&
                next.type != TokenType::kKwHaving && next.type != TokenType::kKwOrder &&
                next.type != TokenType::kKwLimit && next.type != TokenType::kKwUnion &&
                next.type != TokenType::kKwIntersect && next.type != TokenType::kKwExcept &&
                !iequals(text, "offset") && !iequals(text, "fetch") && !iequals(text, "window");
            if (element_follows) {
                advance();
            }
        }
        group_by.push_back(parse_group_by_item());
        while (match(TokenType::kComma)) {
            group_by.push_back(parse_group_by_item());
        }
    }
    Expression* having = nullptr;
    if (match(TokenType::kKwHaving)) {
        having = parse_expr();
    }
    std::vector<WindowDefinition*> window_definitions;
    if (at_soft("window")) {
        advance();
        do {
            const SourceLocation window_loc = loc_of(cur());
            const NamePart name = parse_name_part();
            expect(TokenType::kKwAs, "AS in WINDOW clause");
            Window* window = parse_window();
            window_definitions.push_back(make<WindowDefinition>(window_loc, name, window));
        } while (match(TokenType::kComma));
    }
    return make<QuerySpecification>(loc,
                                    distinct,
                                    make_list(items),
                                    make_list(from),
                                    where,
                                    having,
                                    make_list(group_by),
                                    group_by_distinct,
                                    make_list(window_definitions));
}

Node* Parser::parse_values() {
    const SourceLocation loc = loc_of(advance()); // VALUES
    std::vector<Expression*> rows;
    do {
        if (at(TokenType::kLParen)) {
            const SourceLocation row_loc = loc_of(advance());
            std::vector<Expression*> items;
            if (!at(TokenType::kRParen)) {
                items.push_back(parse_expr());
                while (match(TokenType::kComma)) {
                    items.push_back(parse_expr());
                }
            }
            expect(TokenType::kRParen, "')' after VALUES row");
            rows.push_back(make<Row>(row_loc, make_list(items)));
        } else {
            rows.push_back(parse_expr());
        }
    } while (match(TokenType::kComma));
    return make<Values>(loc, make_list(rows));
}

SelectItem* Parser::parse_select_item() {
    const SourceLocation loc = loc_of(cur());
    if (match(TokenType::kStar)) {
        return make<AllColumns>(loc, AstList<NamePart>{}, nullptr, AstList<NamePart>{});
    }
    // prefix.* — only when a name chain is directly followed by '.' '*'
    if (at_name()) {
        const uint32_t saved = pos_;
        std::vector<NamePart> parts;
        parts.push_back(parse_name_part());
        bool prefixed_star = false;
        while (at(TokenType::kDot)) {
            if (peek(1).type == TokenType::kStar) {
                advance(); // '.'
                advance(); // '*'
                prefixed_star = true;
                break;
            }
            advance(); // '.'
            if (!at_name()) {
                break;
            }
            parts.push_back(parse_name_part());
        }
        if (prefixed_star) {
            return make<AllColumns>(loc, make_list(parts), nullptr, AstList<NamePart>{});
        }
        pos_ = saved;
    }

    Expression* expr = parse_expr();
    // (expression).* — the target must be a primary-level expression so that
    // `SELECT 1 + A.*` stays a syntax error, matching Trino.
    if (at(TokenType::kDot) && peek(1).type == TokenType::kStar &&
        (expr->kind == NodeKind::kRow || expr->kind == NodeKind::kIdentifier ||
         expr->kind == NodeKind::kDereference || expr->kind == NodeKind::kSubqueryExpression)) {
        advance(); // '.'
        advance(); // '*'
        std::vector<NamePart> aliases;
        if (match(TokenType::kKwAs)) {
            expect(TokenType::kLParen, "'(' after AS");
            aliases.push_back(parse_name_part());
            while (match(TokenType::kComma)) {
                aliases.push_back(parse_name_part());
            }
            expect(TokenType::kRParen, "')' after column aliases");
        }
        return make<AllColumns>(loc, AstList<NamePart>{}, expr, make_list(aliases));
    }

    NamePart alias{};
    bool has_alias = false;
    if (match(TokenType::kKwAs)) {
        alias = parse_name_part();
        has_alias = true;
    } else if (at_name()) {
        alias = parse_name_part();
        has_alias = true;
    }
    return make<SingleColumn>(loc, expr, alias, has_alias);
}

Expression* Parser::parse_group_by_item() {
    // GROUP BY AUTO derives the grouping sets from the select items.
    if (at_soft("auto")) {
        const SourceLocation loc = loc_of(advance());
        return make<GroupingAuto>(loc);
    }
    // GROUP BY () is a single empty grouping set.
    if (at(TokenType::kLParen) && peek(1).type == TokenType::kRParen) {
        const SourceLocation loc = loc_of(advance());
        advance();
        return make<Row>(loc, AstList<Expression*>{});
    }
    if (at(TokenType::kKwGrouping) && peek(1).type == TokenType::kIdentifier &&
        iequals(peek(1).text(source_), "sets")) {
        const SourceLocation loc = loc_of(advance());
        advance(); // sets
        expect(TokenType::kLParen, "'(' after GROUPING SETS");
        std::vector<Expression*> sets;
        do {
            if (at(TokenType::kLParen)) {
                const SourceLocation set_loc = loc_of(advance());
                std::vector<Expression*> items;
                if (!at(TokenType::kRParen)) {
                    items.push_back(parse_expr());
                    while (match(TokenType::kComma)) {
                        items.push_back(parse_expr());
                    }
                }
                expect(TokenType::kRParen, "')' in GROUPING SETS");
                sets.push_back(make<Row>(set_loc, make_list(items)));
            } else {
                sets.push_back(parse_expr());
            }
        } while (match(TokenType::kComma));
        expect(TokenType::kRParen, "')' after GROUPING SETS");
        return make<FunctionCall>(loc,
                                  single_name("GROUPING SETS"),
                                  false,
                                  false,
                                  make_list(sets),
                                  nullptr,
                                  AstList<SortItem*>{},
                                  nullptr,
                                  NamePart{},
                                  false);
    }
    if (at(TokenType::kKwCube) || at(TokenType::kKwRollup)) {
        const Token& token = advance();
        const SourceLocation loc = loc_of(token);
        expect(TokenType::kLParen, "'(' after CUBE/ROLLUP");
        std::vector<Expression*> args;
        if (!at(TokenType::kRParen)) {
            args.push_back(parse_expr());
            while (match(TokenType::kComma)) {
                args.push_back(parse_expr());
            }
        }
        expect(TokenType::kRParen, "')' after CUBE/ROLLUP");
        return make<FunctionCall>(loc,
                                  single_name(token.text(source_)),
                                  false,
                                  false,
                                  make_list(args),
                                  nullptr,
                                  AstList<SortItem*>{},
                                  nullptr,
                                  NamePart{},
                                  false);
    }
    return parse_expr();
}

std::vector<SortItem*> Parser::parse_sort_list() {
    std::vector<SortItem*> items;
    do {
        const SourceLocation loc = loc_of(cur());
        Expression* key = parse_expr();
        Ordering ordering = Ordering::kUnspecified;
        if (match_soft("asc")) {
            ordering = Ordering::kAsc;
        } else if (match_soft("desc")) {
            ordering = Ordering::kDesc;
        }
        NullOrdering null_ordering = NullOrdering::kUnspecified;
        if (at_soft("nulls")) {
            advance();
            if (match_soft("first")) {
                null_ordering = NullOrdering::kFirst;
            } else if (match_soft("last")) {
                null_ordering = NullOrdering::kLast;
            } else {
                fail(cur(), "expected FIRST or LAST after NULLS");
            }
        }
        items.push_back(make<SortItem>(loc, key, ordering, null_ordering));
    } while (match(TokenType::kComma));
    return items;
}

} // namespace pl::prism::syntax
