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

// Special expression forms with keyword-driven internal syntax: TRIM,
// SUBSTRING/POSITION/OVERLAY in their FROM/IN/PLACING variants, CASE, CAST,
// INTERVAL, and type names (shared with future DDL column definitions).

#include <utility>

#include "cpp/pl/prism/syntax/parser.h"

namespace pl::prism::syntax {

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

Expression* Parser::parse_case(SourceLocation loc) {
    advance(); // CASE
    Expression* operand = nullptr;
    if (!at(TokenType::kKwWhen)) {
        operand = parse_expr();
    }
    std::vector<WhenClause*> whens;
    while (at(TokenType::kKwWhen)) {
        const SourceLocation when_loc = loc_of(advance());
        // A simple CASE allows partial predicates as WHEN operands:
        // CASE x WHEN > 5 THEN ... WHEN BETWEEN 1 AND 4 THEN ...
        Expression* when = nullptr;
        if (operand != nullptr) {
            when = parse_predicate_tail(nullptr);
        }
        if (when == nullptr) {
            when = parse_expr();
        }
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

std::string_view Parser::parse_interval_unit() {
    const Token& start = cur();
    parse_name_part();
    // Field precision: YEAR(1), DAY(1) TO SECOND(2), SECOND(1, 2).
    if (!at(TokenType::kLParen)) {
        return start.text(source_);
    }
    advance();
    const Token& precision = cur();
    if (precision.type != TokenType::kNumber) {
        fail(precision, "expected field precision in INTERVAL");
    }
    advance();
    if (match(TokenType::kComma)) {
        const Token& scale = cur();
        if (scale.type != TokenType::kNumber) {
            fail(scale, "expected fractional seconds precision in INTERVAL");
        }
        advance();
    }
    const Token& end = expect(TokenType::kRParen, "')' after INTERVAL field precision");
    return source_.substr(start.offset, end.offset + end.length - start.offset);
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
    const std::string_view from_unit = parse_interval_unit();
    std::string_view to_unit;
    if (match(TokenType::kKwTo)) {
        to_unit = parse_interval_unit();
    }
    return make<IntervalLiteral>(loc, negative, value.text(source_), from_unit, to_unit);
}

char Parser::parse_uescape_char() {
    const Token& token = cur();
    if (token.type != TokenType::kString) {
        fail(token, "expected string literal after UESCAPE");
    }
    const std::string_view raw = token.text(source_);
    // Decode the quoted body; a doubled quote decodes to one quote.
    char result = '\0';
    uint32_t count = 0;
    for (size_t i = 1; i + 1 < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\'' && raw[i + 1] == '\'') {
            ++i;
        }
        result = c;
        ++count;
    }
    if (count == 0) {
        fail(token, "empty Unicode escape character");
    }
    if (count != 1) {
        fail(token, "invalid Unicode escape character: must be a single character");
    }
    advance();
    const char c = result;
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    if (hex || c == '+' || c == '\'' || space) {
        fail(token, "invalid Unicode escape character");
    }
    return c;
}

} // namespace pl::prism::syntax
