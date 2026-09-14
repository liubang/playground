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

// Expression parsing: the precedence chain from OR down to primary
// expressions, special forms (TRIM/SUBSTRING/POSITION/OVERLAY), function
// calls, window specifications, CASE, CAST, type names and intervals.

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
                // Quantified comparison: value op ANY | SOME | ALL (subquery).
                if (at_soft("any") || at_soft("some") || at(TokenType::kKwAll)) {
                    Quantifier quantifier = Quantifier::kAny;
                    if (at_soft("some")) {
                        quantifier = Quantifier::kSome;
                    } else if (at(TokenType::kKwAll)) {
                        quantifier = Quantifier::kAll;
                    }
                    advance();
                    expect(TokenType::kLParen, "'(' after quantifier");
                    Query* subquery = parse_query();
                    expect(TokenType::kRParen, "')' after quantified subquery");
                    left =
                        make<QuantifiedComparisonExpression>(loc, op, left, quantifier, subquery);
                    continue;
                }
                left = make<ComparisonExpression>(loc, op, left, parse_concat(), false);
                continue;
            }
            case TokenType::kKwIs: {
                advance();
                const bool negated = match(TokenType::kKwNot);
                if (match(TokenType::kKwNull)) {
                    left = make<IsNullPredicate>(loc, left, negated);
                    continue;
                }
                if (match(TokenType::kKwDistinct)) {
                    expect(TokenType::kKwFrom, "FROM after DISTINCT");
                    left = make<ComparisonExpression>(
                        loc, ComparisonOp::kIsDistinctFrom, left, parse_concat(), negated);
                    continue;
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
                    left = make<BooleanTestPredicate>(loc, left, test, negated);
                    continue;
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
                return left;
            }
            advance();
            negated = true;
        }

        if (match(TokenType::kKwBetween)) {
            Expression* lo = parse_concat();
            expect(TokenType::kKwAnd, "AND in BETWEEN predicate");
            Expression* hi = parse_concat();
            left = make<BetweenPredicate>(loc, left, lo, hi, negated);
            continue;
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
            left = make<InPredicate>(loc, left, value_list, negated);
            continue;
        }
        if (at(TokenType::kKwLike) || at_soft("ilike")) {
            const bool ci = at(TokenType::kIdentifier);
            advance();
            Expression* pattern = parse_concat();
            Expression* escape = nullptr;
            if (match(TokenType::kKwEscape)) {
                escape = parse_concat();
            }
            left = make<LikePredicate>(loc, left, pattern, escape, ci, negated);
            continue;
        }
        if (negated) {
            fail(cur(), "expected BETWEEN, IN or LIKE after NOT");
        }
        return left;
    }
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
        if (match(TokenType::kDot)) {
            const SourceLocation loc = loc_of(cur());
            const NamePart field = parse_name_part();
            expr = make<DereferenceExpression>(loc, expr, field.text, field.quoted);
            continue;
        }
        if (at(TokenType::kKwAt)) {
            const SourceLocation at_loc = loc_of(advance());
            if (!match_soft("time") || !match_soft("zone")) {
                fail(cur(), "expected TIME ZONE after AT");
            }
            expr = make<AtTimeZone>(at_loc, expr, parse_primary());
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
        case TokenType::kBinaryString:
            advance();
            return make<StringLiteral>(loc, token.text(source_));
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
        // DATE 'x' / TIME 'x' / TIMESTAMP 'x'
        if ((iequals(word, "date") || iequals(word, "time") || iequals(word, "timestamp")) &&
            peek(1).type == TokenType::kString) {
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

Expression* Parser::parse_trim(SourceLocation loc) {
    expect(TokenType::kLParen, "'(' after TRIM");

    TrimSpec spec = TrimSpec::kBoth;
    if (at_soft("both")) {
        advance();
    } else if (at_soft("leading")) {
        advance();
        spec = TrimSpec::kLeading;
    } else if (at_soft("trailing")) {
        advance();
        spec = TrimSpec::kTrailing;
    }

    // The trim-chars operand is parsed one precedence level below predicates so
    // that TRIM('a' FROM x) does not misread FROM as part of an expression.
    Expression* first = nullptr;
    if (!at(TokenType::kKwFrom) && !at(TokenType::kRParen)) {
        first = parse_concat();
    }
    if (match(TokenType::kKwFrom)) {
        Expression* source = parse_expr();
        expect(TokenType::kRParen, "')' after TRIM");
        return make<TrimExpression>(loc, spec, source, first);
    }
    expect(TokenType::kRParen, "')' after TRIM");
    if (first == nullptr) {
        fail(cur(), "expected expression in TRIM");
    }
    return make<TrimExpression>(loc, spec, first, nullptr);
}

Expression* Parser::parse_substring_special(SourceLocation loc, std::string_view word) {
    advance(); // SUBSTRING
    expect(TokenType::kLParen, "'(' after SUBSTRING");

    Expression* value = parse_expr();
    if (match(TokenType::kKwFrom)) {
        Expression* start = parse_expr();
        Expression* length = nullptr;
        if (match(TokenType::kKwFor)) {
            length = parse_expr();
        }
        expect(TokenType::kRParen, "')' after SUBSTRING");
        return make<SubstringExpression>(loc, value, start, length);
    }

    // The comma form parses as an ordinary function call.
    std::vector<Expression*> args{value};
    while (match(TokenType::kComma)) {
        args.push_back(parse_expr());
    }
    expect(TokenType::kRParen, "')' after SUBSTRING");
    return finish_function_call(loc, single_name(word), false, false, std::move(args), {});
}

Expression* Parser::parse_position_special(SourceLocation loc) {
    advance(); // POSITION
    expect(TokenType::kLParen, "'(' after POSITION");

    // The needle must not swallow the IN keyword of the special form, so it is
    // parsed one precedence level below predicates.
    Expression* needle = parse_concat();
    expect(TokenType::kKwIn, "IN in POSITION");
    Expression* haystack = parse_expr();
    expect(TokenType::kRParen, "')' after POSITION");
    return make<PositionExpression>(loc, needle, haystack);
}

Expression* Parser::parse_overlay_special(SourceLocation loc) {
    advance(); // OVERLAY
    expect(TokenType::kLParen, "'(' after OVERLAY");

    Expression* value = parse_concat();
    if (!match_soft("placing")) {
        fail(cur(), "expected PLACING in OVERLAY");
    }
    Expression* replacement = parse_expr();
    expect(TokenType::kKwFrom, "FROM in OVERLAY");
    Expression* start = parse_expr();
    Expression* length = nullptr;
    if (match(TokenType::kKwFor)) {
        length = parse_expr();
    }
    expect(TokenType::kRParen, "')' after OVERLAY");
    return make<OverlayExpression>(loc, value, replacement, start, length);
}

Expression* Parser::parse_function_call(SourceLocation loc, AstList<NamePart> name) {
    expect(TokenType::kLParen, "'(' after function name");
    bool distinct = false;
    bool wildcard = false;
    std::vector<Expression*> args;
    std::vector<SortItem*> order_by;
    if (match(TokenType::kStar)) {
        wildcard = true;
        expect(TokenType::kRParen, "')' after '*'");
    } else if (match(TokenType::kRParen)) {
        // no arguments
    } else {
        distinct = match(TokenType::kKwDistinct);
        args.push_back(parse_expr());
        while (match(TokenType::kComma)) {
            args.push_back(parse_expr());
        }
        // Aggregate-internal ORDER BY, e.g. array_agg(x ORDER BY y).
        if (match(TokenType::kKwOrder)) {
            expect(TokenType::kKwBy, "BY after ORDER");
            order_by = parse_sort_list();
        }
        expect(TokenType::kRParen, "')' after function arguments");
    }
    return finish_function_call(
        loc, name, distinct, wildcard, std::move(args), std::move(order_by));
}

Expression* Parser::finish_function_call(SourceLocation loc,
                                         AstList<NamePart> name,
                                         bool distinct,
                                         bool wildcard,
                                         std::vector<Expression*> args,
                                         std::vector<SortItem*> order_by) {
    // FILTER (WHERE ...) on aggregates.
    Expression* filter = nullptr;
    if (at_soft("filter")) {
        advance();
        expect(TokenType::kLParen, "'(' after FILTER");
        expect(TokenType::kKwWhere, "WHERE in FILTER");
        filter = parse_expr();
        expect(TokenType::kRParen, "')' after FILTER");
    }

    // OVER (...) or OVER window_name.
    Window* window = nullptr;
    NamePart window_ref{};
    bool has_window_ref = false;
    if (at_soft("over")) {
        advance();
        if (at(TokenType::kLParen)) {
            window = parse_window();
        } else if (at_name()) {
            window_ref = parse_name_part();
            has_window_ref = true;
        } else {
            fail(cur(), "expected window specification or window name after OVER");
        }
    }
    return make<FunctionCall>(loc,
                              name,
                              distinct,
                              wildcard,
                              make_list(args),
                              filter,
                              make_list(order_by),
                              window,
                              window_ref,
                              has_window_ref);
}

Window* Parser::parse_window() {
    const SourceLocation loc = loc_of(cur());
    expect(TokenType::kLParen, "'(' after OVER");
    // A window specification may derive from a named window defined in the
    // WINDOW clause: (someWindow ORDER BY b).
    NamePart existing_window{};
    bool has_existing_window = false;
    if (at_name() && !at_soft("partition") && !at_soft("rows") && !at_soft("range") &&
        !at_soft("groups")) {
        existing_window = parse_name_part();
        has_existing_window = true;
    }
    std::vector<Expression*> partition_by;
    if (match_soft("partition")) {
        expect(TokenType::kKwBy, "BY after PARTITION");
        partition_by.push_back(parse_expr());
        while (match(TokenType::kComma)) {
            partition_by.push_back(parse_expr());
        }
    }
    std::vector<SortItem*> order_by;
    if (match(TokenType::kKwOrder)) {
        expect(TokenType::kKwBy, "BY after ORDER");
        order_by = parse_sort_list();
    }
    WindowFrame* frame = nullptr;
    if (at_soft("rows") || at_soft("range") || at_soft("groups")) {
        frame = parse_frame();
    }
    expect(TokenType::kRParen, "')' after window specification");
    return make<Window>(loc,
                        existing_window,
                        has_existing_window,
                        make_list(partition_by),
                        make_list(order_by),
                        frame);
}

WindowFrame* Parser::parse_frame() {
    const SourceLocation loc = loc_of(cur());
    FrameType frame_type = FrameType::kRows;
    if (match_soft("range")) {
        frame_type = FrameType::kRange;
    } else if (match_soft("groups")) {
        frame_type = FrameType::kGroups;
    } else {
        advance(); // "rows"
    }
    if (match(TokenType::kKwBetween)) {
        const FrameBound start = parse_frame_bound();
        expect(TokenType::kKwAnd, "AND in window frame");
        const FrameBound end = parse_frame_bound();
        return make<WindowFrame>(loc, frame_type, start, end, true);
    }
    const FrameBound start = parse_frame_bound();
    return make<WindowFrame>(loc, frame_type, start, FrameBound{}, false);
}

FrameBound Parser::parse_frame_bound() {
    if (match_soft("unbounded")) {
        if (match_soft("preceding")) {
            return FrameBound{FrameBoundType::kUnboundedPreceding, nullptr};
        }
        if (!match_soft("following")) {
            fail(cur(), "expected PRECEDING or FOLLOWING after UNBOUNDED");
        }
        return FrameBound{FrameBoundType::kUnboundedFollowing, nullptr};
    }
    if (at(TokenType::kKwCurrent) || at_soft("current")) {
        advance();
        if (!match_soft("row")) {
            fail(cur(), "expected ROW after CURRENT");
        }
        return FrameBound{FrameBoundType::kCurrentRow, nullptr};
    }
    Expression* value = parse_expr();
    if (match_soft("preceding")) {
        return FrameBound{FrameBoundType::kPreceding, value};
    }
    if (!match_soft("following")) {
        fail(cur(), "expected PRECEDING or FOLLOWING in window frame bound");
    }
    return FrameBound{FrameBoundType::kFollowing, value};
}

Expression* Parser::parse_case(SourceLocation loc) {
    advance(); // CASE
    Expression* operand = nullptr;
    if (!at(TokenType::kKwWhen)) {
        operand = parse_expr();
    }
    std::vector<WhenClause*> whens;
    while (at(TokenType::kKwWhen)) {
        const SourceLocation when_loc = loc_of(advance());
        Expression* when = parse_expr();
        expect(TokenType::kKwThen, "THEN in CASE expression");
        Expression* result = parse_expr();
        whens.push_back(make<WhenClause>(when_loc, when, result));
    }
    if (whens.empty()) {
        fail(cur(), "expected WHEN in CASE expression");
    }
    Expression* else_result = nullptr;
    if (match(TokenType::kKwElse)) {
        else_result = parse_expr();
    }
    expect(TokenType::kKwEnd, "END in CASE expression");
    if (operand != nullptr) {
        return make<SimpleCaseExpression>(loc, operand, make_list(whens), else_result);
    }
    return make<SearchedCaseExpression>(loc, make_list(whens), else_result);
}

Expression* Parser::parse_cast(SourceLocation loc, bool try_cast) {
    expect(TokenType::kLParen, "'(' after CAST");
    Expression* expr = parse_expr();
    expect(TokenType::kKwAs, "AS in CAST");
    TypeName* type = parse_type();
    expect(TokenType::kRParen, "')' after CAST");
    return make<Cast>(loc, expr, type, try_cast);
}

TypeName* Parser::parse_type() {
    const Token& start = cur();
    const SourceLocation loc = loc_of(start);
    if (!at(TokenType::kIdentifier) && !at(TokenType::kKwArray)) {
        fail(start, "expected type name");
    }
    advance();
    std::string_view name = start.text(source_);
    // Multi-word type: DOUBLE PRECISION. The name is kept as a raw source
    // slice spanning both tokens.
    if (iequals(name, "double") && at_soft("precision")) {
        const Token& second = advance();
        name = source_.substr(start.offset, second.offset + second.length - start.offset);
    }

    std::vector<TypeName*> type_args;
    std::vector<std::string_view> num_args;
    std::vector<NamePart> field_names;

    if (iequals(name, "array")) {
        expect(TokenType::kLParen, "'(' after ARRAY");
        type_args.push_back(parse_type());
        expect(TokenType::kRParen, "')' after ARRAY element type");
    } else if (iequals(name, "map")) {
        expect(TokenType::kLParen, "'(' after MAP");
        type_args.push_back(parse_type());
        expect(TokenType::kComma, "',' between MAP key and value types");
        type_args.push_back(parse_type());
        expect(TokenType::kRParen, "')' after MAP types");
    } else if (iequals(name, "row")) {
        expect(TokenType::kLParen, "'(' after ROW");
        if (!at(TokenType::kRParen)) {
            do {
                if (at(TokenType::kIdentifier) && peek(1).type == TokenType::kIdentifier) {
                    field_names.push_back(parse_name_part());
                } else {
                    field_names.push_back(NamePart{});
                }
                type_args.push_back(parse_type());
            } while (match(TokenType::kComma));
        }
        expect(TokenType::kRParen, "')' after ROW fields");
    } else if (match(TokenType::kLParen)) {
        do {
            const Token& num = cur();
            if (num.type != TokenType::kNumber) {
                fail(num, "expected numeric type parameter");
            }
            advance();
            num_args.push_back(num.text(source_));
        } while (match(TokenType::kComma));
        expect(TokenType::kRParen, "')' after type parameters");
    }

    TimeZoneSpec tz = TimeZoneSpec::kNone;
    if (match(TokenType::kKwWith)) {
        if (!match_soft("time") || !match_soft("zone")) {
            fail(cur(), "expected TIME ZONE after WITH");
        }
        tz = TimeZoneSpec::kWith;
    } else if (match_soft("without")) {
        if (!match_soft("time") || !match_soft("zone")) {
            fail(cur(), "expected TIME ZONE after WITHOUT");
        }
        tz = TimeZoneSpec::kWithout;
    }

    return make<TypeName>(
        loc, name, make_list(type_args), make_list(num_args), make_list(field_names), tz);
}

Expression* Parser::parse_interval(SourceLocation loc) {
    const bool negative = match(TokenType::kMinus);
    if (!negative) {
        match(TokenType::kPlus);
    }
    const Token& value = cur();
    if (value.type != TokenType::kString) {
        fail(value, "expected string literal in INTERVAL");
    }
    advance();
    const std::string_view from_unit = parse_name_part().text;
    std::string_view to_unit;
    if (match(TokenType::kKwTo)) {
        to_unit = parse_name_part().text;
    }
    return make<IntervalLiteral>(loc, negative, value.text(source_), from_unit, to_unit);
}

} // namespace pl::prism::syntax
