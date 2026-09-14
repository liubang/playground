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

// Relation parsing: joins with their criteria, and table primaries (table
// references, table subqueries, parenthesized joins, UNNEST, LATERAL,
// TABLESAMPLE) with aliasing. P2 additions such as JSON_TABLE and pattern
// recognition (MATCH_RECOGNIZE) are further table primaries and land here.

#include "cpp/pl/prism/syntax/parser.h"

namespace pl::prism::syntax {

Relation* Parser::parse_relation() {
    Relation* left = parse_relation_primary();
    for (;;) {
        const SourceLocation loc = loc_of(cur());
        const bool natural = match(TokenType::kKwNatural);

        JoinType join_type = JoinType::kInner;
        if (match(TokenType::kKwCross)) {
            expect(TokenType::kKwJoin, "JOIN after CROSS");
            join_type = JoinType::kCross;
        } else if (match(TokenType::kKwInner)) {
            expect(TokenType::kKwJoin, "JOIN after INNER");
            join_type = JoinType::kInner;
        } else if (match(TokenType::kKwLeft)) {
            match(TokenType::kKwOuter);
            expect(TokenType::kKwJoin, "JOIN after LEFT");
            join_type = JoinType::kLeft;
        } else if (match(TokenType::kKwRight)) {
            match(TokenType::kKwOuter);
            expect(TokenType::kKwJoin, "JOIN after RIGHT");
            join_type = JoinType::kRight;
        } else if (match(TokenType::kKwFull)) {
            match(TokenType::kKwOuter);
            expect(TokenType::kKwJoin, "JOIN after FULL");
            join_type = JoinType::kFull;
        } else if (match(TokenType::kKwJoin)) {
            join_type = JoinType::kInner;
        } else {
            if (natural) {
                fail(cur(), "expected JOIN after NATURAL");
            }
            return left;
        }

        Relation* right = parse_relation_primary();
        Expression* on = nullptr;
        std::vector<NamePart> using_columns;
        if (join_type != JoinType::kCross) {
            if (match(TokenType::kKwOn)) {
                on = parse_expr();
            } else if (match(TokenType::kKwUsing)) {
                expect(TokenType::kLParen, "'(' after USING");
                using_columns.push_back(parse_name_part());
                while (match(TokenType::kComma)) {
                    using_columns.push_back(parse_name_part());
                }
                expect(TokenType::kRParen, "')' after USING columns");
            } else if (!natural) {
                fail(cur(), "expected ON or USING join criteria");
            }
        }
        left = make<Join>(loc, join_type, natural, left, right, on, make_list(using_columns));
    }
}

Relation* Parser::parse_relation_primary() {
    const SourceLocation loc = loc_of(cur());
    Relation* relation = nullptr;
    if (at(TokenType::kLParen)) {
        if (is_query_start(1)) {
            advance();
            Query* query = parse_query();
            expect(TokenType::kRParen, "')' after table subquery");
            relation = make<TableSubquery>(loc, query);
        } else {
            advance();
            relation = parse_relation();
            expect(TokenType::kRParen, "')' after parenthesized relation");
        }
    } else if (at(TokenType::kKwUnnest)) {
        advance();
        expect(TokenType::kLParen, "'(' after UNNEST");
        std::vector<Expression*> expressions;
        expressions.push_back(parse_expr());
        while (match(TokenType::kComma)) {
            expressions.push_back(parse_expr());
        }
        expect(TokenType::kRParen, "')' after UNNEST");
        bool with_ordinality = false;
        if (match(TokenType::kKwWith)) {
            if (!match_soft("ordinality")) {
                fail(cur(), "expected ORDINALITY after WITH");
            }
            with_ordinality = true;
        }
        relation = make<Unnest>(loc, make_list(expressions), with_ordinality);
    } else if (at_soft("lateral")) {
        advance();
        expect(TokenType::kLParen, "'(' after LATERAL");
        Query* query = parse_query();
        expect(TokenType::kRParen, "')' after LATERAL subquery");
        relation = make<Lateral>(loc, query);
    } else {
        relation = make<Table>(loc, parse_qualified_name());
        if (at_soft("tablesample")) {
            advance();
            SampleType sample_type = SampleType::kBernoulli;
            if (match_soft("bernoulli")) {
                sample_type = SampleType::kBernoulli;
            } else if (match_soft("system")) {
                sample_type = SampleType::kSystem;
            } else {
                fail(cur(), "expected BERNOULLI or SYSTEM after TABLESAMPLE");
            }
            expect(TokenType::kLParen, "'(' after TABLESAMPLE type");
            Expression* percentage = parse_expr();
            expect(TokenType::kRParen, "')' after TABLESAMPLE percentage");
            relation = make<TableSample>(loc, relation, sample_type, percentage);
        }
    }
    return maybe_alias(relation);
}

Relation* Parser::maybe_alias(Relation* relation) {
    NamePart alias{};
    bool has_alias = false;
    if (match(TokenType::kKwAs)) {
        alias = parse_name_part();
        has_alias = true;
    } else if (at_name() && !at_soft("offset") && !at_soft("fetch") && !at_soft("window")) {
        alias = parse_name_part();
        has_alias = true;
    }
    if (!has_alias) {
        return relation;
    }
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
    return make<AliasedRelation>(relation->location, relation, alias, make_list(columns));
}

} // namespace pl::prism::syntax
