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

// The expression precedence chain, from OR down to primary expressions, plus
// parenthesized forms (row constructors, subqueries, lambdas) and the
// identifier-led primary dispatch. Special forms live in
// parser_special_form.cpp, function calls in parser_function.cpp.

#include <utility>

#include "cpp/pl/prism/syntax/parser.h"

namespace pl::prism::syntax {

Expression* Parser::parse_expr() {
    return parse_or();
}

Expression* Parser::parse_or() {
    Expression* left = parse_and();
    while (at(TokenType::kKwOr)) {
        const SourceLocation loc = loc_of(advance());
        left = make<LogicalBinaryExpression>(loc, LogicalOp::kOr, left, parse_and());
    }
    return left;
}

Expression* Parser::parse_and() {
    Expression* left = parse_not();
    while (at(TokenType::kKwAnd)) {
        const SourceLocation loc = loc_of(advance());
        left = make<LogicalBinaryExpression>(loc, LogicalOp::kAnd, left, parse_not());
    }
    return left;
}

Expression* Parser::parse_not() {
    if (at(TokenType::kKwNot)) {
        const SourceLocation loc = loc_of(advance());
        return make<NotExpression>(loc, parse_not());
    }
    return parse_predicate();
}

Expression* Parser::parse_predicate() {
    Expression* left = parse_concat();
    for (;;) {
        Expression* next = parse_predicate_tail(left);
        if (next == nullptr) {
            return left;
        }
        left = next;
    }
}

Expression* Parser::parse_predicate_tail(Expression* left) {
    const SourceLocation loc = loc_of(cur());

    switch (cur().type) {
        case TokenType::kEq:
        case TokenType::kNeq:
        case TokenType::kLt:
        case TokenType::kGt:
        case TokenType::kLte:
        case TokenType::kGte: {
            ComparisonOp op = ComparisonOp::kEqual;
            switch (cur().type) {
                case TokenType::kEq:
                    op = ComparisonOp::kEqual;
                    break;
                case TokenType::kNeq:
                    op = ComparisonOp::kNotEqual;
                    break;
                case TokenType::kLt:
                    op = ComparisonOp::kLessThan;
                    break;
                case TokenType::kGt:
                    op = ComparisonOp::kGreaterThan;
                    break;
                case TokenType::kLte:
                    op = ComparisonOp::kLessThanOrEqual;
                    break;
                default:
                    op = ComparisonOp::kGreaterThanOrEqual;
                    break;
            }
            advance();
            // Quantified comparison: value op ANY | SOME | ALL (subquery). The
            // parenthesis guard keeps `x = any` (column named any) parseable.
            if ((at_soft("any") || at_soft("some") || at_soft("all")) &&
                peek(1).type == TokenType::kLParen) {
                Quantifier quantifier = Quantifier::kAny;
                if (at_soft("some")) {
                    quantifier = Quantifier::kSome;
                } else if (at_soft("all")) {
                    quantifier = Quantifier::kAll;
                }
                advance();
                expect(TokenType::kLParen, "'(' after quantifier");
                Query* subquery = parse_query();
                expect(TokenType::kRParen, "')' after quantified subquery");
                return make<QuantifiedComparisonExpression>(loc, op, left, quantifier, subquery);
            }
            return make<ComparisonExpression>(loc, op, left, parse_concat(), false);
        }
        case TokenType::kKwIs: {
            advance();
            const bool negated = match(TokenType::kKwNot);
            if (match(TokenType::kKwNull)) {
                return make<IsNullPredicate>(loc, left, negated);
            }
            if (match(TokenType::kKwDistinct)) {
                expect(TokenType::kKwFrom, "FROM after DISTINCT");
                return make<ComparisonExpression>(
                    loc, ComparisonOp::kIsDistinctFrom, left, parse_concat(), negated);
            }
            if (at(TokenType::kKwTrue) || at(TokenType::kKwFalse) || at_soft("unknown")) {
                BooleanTestType test = BooleanTestType::kTrue;
                if (match(TokenType::kKwTrue)) {
                    test = BooleanTestType::kTrue;
                } else if (match(TokenType::kKwFalse)) {
                    test = BooleanTestType::kFalse;
                } else {
                    advance();
                    test = BooleanTestType::kUnknown;
                }
                return make<BooleanTestPredicate>(loc, left, test, negated);
            }
            fail(cur(), "expected NULL, DISTINCT, TRUE, FALSE or UNKNOWN after IS");
        }
        default:
            break;
    }

    // [NOT] BETWEEN / [NOT] IN / [NOT] LIKE / [NOT] ILIKE
    bool negated = false;
    if (at(TokenType::kKwNot)) {
        const Token& next = peek(1);
        const bool starts_predicate =
            next.type == TokenType::kKwBetween || next.type == TokenType::kKwIn ||
            next.type == TokenType::kKwLike ||
            (next.type == TokenType::kIdentifier && iequals(next.text(source_), "ilike"));
        if (!starts_predicate) {
            return nullptr;
        }
        advance();
        negated = true;
    }

    if (match(TokenType::kKwBetween)) {
        BetweenSymmetry symmetry = BetweenSymmetry::kAsymmetric;
        if (match_soft("symmetric")) {
            symmetry = BetweenSymmetry::kSymmetric;
        } else {
            match_soft("asymmetric");
        }
        Expression* lo = parse_concat();
        expect(TokenType::kKwAnd, "AND in BETWEEN predicate");
        Expression* hi = parse_concat();
        return make<BetweenPredicate>(loc, left, lo, hi, negated, symmetry);
    }
    if (match(TokenType::kKwIn)) {
        expect(TokenType::kLParen, "'(' after IN");
        Expression* value_list = nullptr;
        if (is_query_start(0)) {
            const SourceLocation sub_loc = loc_of(cur());
            value_list = make<SubqueryExpression>(sub_loc, parse_query());
        } else {
            const SourceLocation list_loc = loc_of(cur());
            std::vector<Expression*> items;
            items.push_back(parse_expr());
            while (match(TokenType::kComma)) {
                items.push_back(parse_expr());
            }
            value_list = make<InListExpression>(list_loc, make_list(items));
        }
        expect(TokenType::kRParen, "')' after IN list");
        return make<InPredicate>(loc, left, value_list, negated);
    }
    if (at(TokenType::kKwLike) || at_soft("ilike")) {
        const bool ci = at(TokenType::kIdentifier);
        advance();
        Expression* pattern = parse_concat();
        Expression* escape = nullptr;
        if (match(TokenType::kKwEscape)) {
            escape = parse_concat();
        }
        return make<LikePredicate>(loc, left, pattern, escape, ci, negated);
    }
    // Row match predicate: value MATCH [UNIQUE] [SIMPLE | PARTIAL | FULL] (q).
    if (!negated && at_soft("match")) {
        const Token& next = peek(1);
        const std::string_view text =
            next.type == TokenType::kIdentifier ? next.text(source_) : std::string_view{};
        const bool starts_match = next.type == TokenType::kLParen ||
                                  next.type == TokenType::kKwFull || iequals(text, "unique") ||
                                  iequals(text, "simple") || iequals(text, "partial");
        if (starts_match) {
            advance();
            const bool unique = match_soft("unique");
            MatchType match_type = MatchType::kUnspecified;
            if (match_soft("simple")) {
                match_type = MatchType::kSimple;
            } else if (match_soft("partial")) {
                match_type = MatchType::kPartial;
            } else if (match(TokenType::kKwFull)) {
                match_type = MatchType::kFull;
            }
            expect(TokenType::kLParen, "'(' after MATCH");
            Query* subquery = parse_query();
            expect(TokenType::kRParen, "')' after MATCH subquery");
            return make<MatchPredicate>(loc, left, unique, match_type, subquery);
        }
    }
    if (negated) {
        fail(cur(), "expected BETWEEN, IN or LIKE after NOT");
    }
    return nullptr;
}

Expression* Parser::parse_concat() {
    Expression* left = parse_additive();
    while (at(TokenType::kConcat)) {
        const SourceLocation loc = loc_of(advance());
        left = make<ArithmeticBinaryExpression>(
            loc, ArithmeticOp::kConcatenate, left, parse_additive());
    }
    return left;
}

Expression* Parser::parse_additive() {
    Expression* left = parse_multiplicative();
    while (at(TokenType::kPlus) || at(TokenType::kMinus)) {
        const ArithmeticOp op = at(TokenType::kPlus) ? ArithmeticOp::kAdd : ArithmeticOp::kSubtract;
        const SourceLocation loc = loc_of(advance());
        left = make<ArithmeticBinaryExpression>(loc, op, left, parse_multiplicative());
    }
    return left;
}

Expression* Parser::parse_multiplicative() {
    Expression* left = parse_unary();
    for (;;) {
        ArithmeticOp op;
        if (at(TokenType::kStar)) {
            op = ArithmeticOp::kMultiply;
        } else if (at(TokenType::kSlash)) {
            op = ArithmeticOp::kDivide;
        } else if (at(TokenType::kPercent)) {
            op = ArithmeticOp::kModulus;
        } else {
            break;
        }
        const SourceLocation loc = loc_of(advance());
        left = make<ArithmeticBinaryExpression>(loc, op, left, parse_unary());
    }
    return left;
}

Expression* Parser::parse_unary() {
    if (at(TokenType::kMinus)) {
        const SourceLocation loc = loc_of(advance());
        return make<ArithmeticUnaryExpression>(loc, true, parse_unary());
    }
    if (at(TokenType::kPlus)) {
        const SourceLocation loc = loc_of(advance());
        return make<ArithmeticUnaryExpression>(loc, false, parse_unary());
    }
    return parse_postfix();
}

Expression* Parser::parse_postfix() {
    Expression* expr = parse_primary();
    for (;;) {
        if (at(TokenType::kLBracket)) {
            const SourceLocation loc = loc_of(advance());
            Expression* index = parse_expr();
            expect(TokenType::kRBracket, "']' after subscript");
            expr = make<SubscriptExpression>(loc, expr, index);
            continue;
        }
        if (at(TokenType::kDot) && peek(1).type == TokenType::kStar) {
            // Leave '.*' unconsumed: it terminates a select item (expr).* and
            // is handled by parse_select_item.
            break;
        }
        if (match(TokenType::kDot)) {
            const SourceLocation loc = loc_of(cur());
            const NamePart field = parse_name_part();
            expr = make<DereferenceExpression>(loc, expr, field.text, field.quoted);
            continue;
        }
        if (at(TokenType::kKwAt)) {
            const SourceLocation at_loc = loc_of(advance());
            if (match_soft("local")) {
                expr = make<AtTimeZone>(at_loc, expr, nullptr, true);
                continue;
            }
            if (!match_soft("time") || !match_soft("zone")) {
                fail(cur(), "expected TIME ZONE or LOCAL after AT");
            }
            expr = make<AtTimeZone>(at_loc, expr, parse_primary(), false);
            continue;
        }
        break;
    }
    return expr;
}

Expression* Parser::parse_primary() {
    const Token& token = cur();
    const SourceLocation loc = loc_of(token);
    switch (token.type) {
        case TokenType::kNumber:
            advance();
            return make<NumberLiteral>(loc, token.text(source_));
        case TokenType::kString:
        case TokenType::kUnicodeString:
        case TokenType::kBinaryString: {
            advance();
            char escape = '\0';
            if (at(TokenType::kKwUescape)) {
                if (token.type != TokenType::kUnicodeString) {
                    fail(cur(), "UESCAPE is only valid after a U& string literal");
                }
                advance();
                escape = parse_uescape_char();
            }
            return make<StringLiteral>(loc, token.text(source_), escape);
        }
        case TokenType::kKwTrue:
            advance();
            return make<BooleanLiteral>(loc, true);
        case TokenType::kKwFalse:
            advance();
            return make<BooleanLiteral>(loc, false);
        case TokenType::kKwNull:
            advance();
            return make<NullLiteral>(loc);
        case TokenType::kKwCase:
            return parse_case(loc);
        case TokenType::kKwCast:
            advance();
            return parse_cast(loc, false);
        case TokenType::kKwExists: {
            advance();
            expect(TokenType::kLParen, "'(' after EXISTS");
            Query* query = parse_query();
            expect(TokenType::kRParen, "')' after EXISTS subquery");
            return make<ExistsPredicate>(loc, query);
        }
        case TokenType::kKwInterval:
            advance();
            return parse_interval(loc);
        case TokenType::kKwArray: {
            advance();
            expect(TokenType::kLBracket, "'[' after ARRAY");
            std::vector<Expression*> items;
            if (!at(TokenType::kRBracket)) {
                items.push_back(parse_expr());
                while (match(TokenType::kComma)) {
                    items.push_back(parse_expr());
                }
            }
            expect(TokenType::kRBracket, "']' after array constructor");
            return make<ArrayConstructor>(loc, make_list(items));
        }
        case TokenType::kLParen:
            return parse_paren(loc);
        case TokenType::kQuestion:
            advance();
            return make<ParameterExpression>(loc);
        case TokenType::kKwListagg:
            return parse_listagg(loc);
        case TokenType::kKwGrouping: {
            advance();
            expect(TokenType::kLParen, "'(' after GROUPING");
            std::vector<Expression*> args;
            args.push_back(parse_expr());
            while (match(TokenType::kComma)) {
                args.push_back(parse_expr());
            }
            expect(TokenType::kRParen, "')' after GROUPING arguments");
            return make<GroupingOperation>(loc, make_list(args));
        }
        case TokenType::kIdentifier:
        case TokenType::kQuotedIdentifier:
            return parse_named_primary(loc);
        default:
            fail(token, "expected expression");
    }
}

bool Parser::looks_like_lambda() const {
    // At '(': checks for '(' name (',' name)* ')' '->'.
    uint32_t idx = pos_ + 1;
    const auto is_name = [&](uint32_t i) {
        return tokens_[i].type == TokenType::kIdentifier ||
               tokens_[i].type == TokenType::kQuotedIdentifier;
    };
    if (!is_name(idx)) {
        return false;
    }
    ++idx;
    while (tokens_[idx].type == TokenType::kComma) {
        if (!is_name(idx + 1)) {
            return false;
        }
        idx += 2;
    }
    return tokens_[idx].type == TokenType::kRParen && tokens_[idx + 1].type == TokenType::kArrow;
}

Expression* Parser::parse_paren(SourceLocation loc) {
    if (looks_like_lambda()) {
        advance(); // '('
        std::vector<NamePart> params;
        params.push_back(parse_name_part());
        while (match(TokenType::kComma)) {
            params.push_back(parse_name_part());
        }
        expect(TokenType::kRParen, "')' after lambda parameters");
        expect(TokenType::kArrow, "'->' after lambda parameters");
        return make<LambdaExpression>(loc, make_list(params), parse_expr());
    }
    if (is_query_start(1)) {
        advance();
        Query* query = parse_query();
        expect(TokenType::kRParen, "')' after subquery");
        return make<SubqueryExpression>(loc, query);
    }
    advance();
    Expression* expr = parse_expr();
    if (match(TokenType::kComma)) {
        std::vector<Expression*> items;
        items.push_back(expr);
        do {
            items.push_back(parse_expr());
        } while (match(TokenType::kComma));
        expect(TokenType::kRParen, "')' after row");
        return make<Row>(loc, make_list(items));
    }
    expect(TokenType::kRParen, "')' after expression");
    return expr;
}

Expression* Parser::parse_named_primary(SourceLocation loc) {
    // single-parameter lambda: x -> body
    if (at(TokenType::kIdentifier) && peek(1).type == TokenType::kArrow) {
        const NamePart param = parse_name_part();
        advance(); // '->'
        return make<LambdaExpression>(loc, single_name(param.text), parse_expr());
    }

    if (at(TokenType::kIdentifier)) {
        const std::string_view word = cur_text();
        // TRY_CAST(x AS t)
        if (iequals(word, "try_cast") && peek(1).type == TokenType::kLParen) {
            advance();
            return parse_cast(loc, true);
        }
        // Any type name directly followed by a string literal is a typed
        // literal in Trino: DATE 'x', TIMESTAMP 'x', DECIMAL '1.0'.
        if (peek(1).type == TokenType::kString) {
            const Token& type_token = advance();
            const Token& value_token = advance();
            return make<TypedLiteral>(loc, type_token.text(source_), value_token.text(source_));
        }
        // ROW(a, b)
        if (iequals(word, "row") && peek(1).type == TokenType::kLParen) {
            advance();
            advance();
            std::vector<Expression*> items;
            if (!at(TokenType::kRParen)) {
                items.push_back(parse_expr());
                while (match(TokenType::kComma)) {
                    items.push_back(parse_expr());
                }
            }
            expect(TokenType::kRParen, "')' after ROW");
            return make<Row>(loc, make_list(items));
        }
        // Special forms that share the function-call shape but have their own
        // AST nodes. All are soft keywords in Trino.
        if (peek(1).type == TokenType::kLParen) {
            if (iequals(word, "trim")) {
                advance();
                return parse_trim(loc);
            }
            if (iequals(word, "substring")) {
                return parse_substring_special(loc, word);
            }
            if (iequals(word, "position")) {
                return parse_position_special(loc);
            }
            if (iequals(word, "overlay")) {
                return parse_overlay_special(loc);
            }
        }
    }

    const AstList<NamePart> parts = parse_qualified_name();
    if (at(TokenType::kLParen)) {
        return parse_function_call(loc, parts);
    }
    Expression* expr = make<Identifier>(loc, parts[0].text, parts[0].quoted);
    for (uint32_t i = 1; i < parts.size; ++i) {
        expr = make<DereferenceExpression>(loc, expr, parts[i].text, parts[i].quoted);
    }
    return expr;
}

} // namespace pl::prism::syntax
