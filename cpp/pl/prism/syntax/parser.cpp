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

#include "cpp/pl/prism/syntax/parser.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "cpp/pl/prism/syntax/lexer.h"

namespace pl::prism::syntax {

namespace {

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

Parser::Parser(std::string_view source) : source_(source) {
    Lexer lexer(source);
    for (;;) {
        Token token = lexer.next_token();
        const bool done = token.type == TokenType::kEof;
        tokens_.push_back(token);
        if (done) {
            break;
        }
    }
    for (const LexError& error : lexer.errors()) {
        errors_.push_back(ParseError{error.offset, error.line, error.column, error.message});
    }
}

ParseResult Parser::parse_statement() {
    if (!errors_.empty()) {
        return ParseResult{nullptr, std::move(errors_)};
    }
    try {
        Node* root = at(TokenType::kKwExplain) ? parse_explain() : parse_query();
        match(TokenType::kSemicolon);
        if (!at(TokenType::kEof)) {
            fail(cur(), "unexpected token after end of statement");
        }
        return ParseResult{root, {}};
    } catch (const ParseAbort&) {
        return ParseResult{nullptr, std::move(errors_)};
    }
}

ParseResult Parser::parse_expression() {
    if (!errors_.empty()) {
        return ParseResult{nullptr, std::move(errors_)};
    }
    try {
        Expression* root = parse_expr();
        if (!at(TokenType::kEof)) {
            fail(cur(), "unexpected token after end of expression");
        }
        return ParseResult{root, {}};
    } catch (const ParseAbort&) {
        return ParseResult{nullptr, std::move(errors_)};
    }
}

// Token navigation.

const Token& Parser::peek(uint32_t n) const {
    size_t idx = static_cast<size_t>(pos_) + n;
    if (idx >= tokens_.size()) {
        idx = tokens_.size() - 1;
    }
    return tokens_[idx];
}

const Token& Parser::advance() {
    const Token& token = tokens_[pos_];
    if (pos_ + 1 < tokens_.size()) {
        ++pos_;
    }
    return token;
}

bool Parser::match(TokenType type) {
    if (at(type)) {
        advance();
        return true;
    }
    return false;
}

const Token& Parser::expect(TokenType type, const char* what) {
    if (!at(type)) {
        std::string message = "expected ";
        message += what;
        message += ", found '";
        message += cur().type == TokenType::kEof ? "end of input" : cur_text();
        message += "'";
        fail(cur(), std::move(message));
    }
    return advance();
}

bool Parser::at_soft(std::string_view word) const {
    return cur().type == TokenType::kIdentifier && iequals(cur_text(), word);
}

bool Parser::match_soft(std::string_view word) {
    if (at_soft(word)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::at_name() const {
    return at(TokenType::kIdentifier) || at(TokenType::kQuotedIdentifier);
}

std::string_view Parser::cur_text() const {
    return cur().text(source_);
}

SourceLocation Parser::loc_of(const Token& token) const {
    return SourceLocation{token.offset, token.line, token.column};
}

bool Parser::is_query_start(uint32_t n) const {
    uint32_t idx = pos_ + n;
    while (tokens_[idx].type == TokenType::kLParen) {
        ++idx;
    }
    const TokenType type = tokens_[idx].type;
    return type == TokenType::kKwSelect || type == TokenType::kKwWith ||
           type == TokenType::kKwValues;
}

void Parser::fail(const Token& token, std::string message) {
    errors_.push_back(ParseError{token.offset, token.line, token.column, std::move(message)});
    throw ParseAbort{};
}

template <typename T> AstList<T> Parser::make_list(const std::vector<T>& items) {
    AstList<T> out;
    out.size = static_cast<uint32_t>(items.size());
    if (!items.empty()) {
        out.data = static_cast<T*>(arena_.allocate(sizeof(T) * items.size(), alignof(T)));
        std::copy(items.begin(), items.end(), out.data);
    }
    return out;
}

AstList<NamePart> Parser::single_name(std::string_view text) {
    NamePart part{text, false};
    AstList<NamePart> out;
    out.size = 1;
    out.data = static_cast<NamePart*>(arena_.allocate(sizeof(NamePart), alignof(NamePart)));
    out.data[0] = part;
    return out;
}

// Names.

NamePart Parser::parse_name_part() {
    const Token& token = cur();
    if (token.type == TokenType::kIdentifier) {
        advance();
        return NamePart{token.text(source_), false};
    }
    if (token.type == TokenType::kQuotedIdentifier) {
        advance();
        return NamePart{token.text(source_), true};
    }
    fail(token, "expected identifier");
}

AstList<NamePart> Parser::parse_qualified_name() {
    std::vector<NamePart> parts;
    parts.push_back(parse_name_part());
    while (match(TokenType::kDot)) {
        parts.push_back(parse_name_part());
    }
    return make_list(parts);
}

// Expressions.

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
                fail(cur(), "expected NULL or DISTINCT FROM after IS");
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

Expression* Parser::parse_function_call(SourceLocation loc, AstList<NamePart> name) {
    expect(TokenType::kLParen, "'(' after function name");
    bool distinct = false;
    bool wildcard = false;
    std::vector<Expression*> args;
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
        expect(TokenType::kRParen, "')' after function arguments");
    }

    Window* window = nullptr;
    if (at_soft("over")) {
        advance();
        window = parse_window();
    }
    return make<FunctionCall>(loc, name, distinct, wildcard, make_list(args), window);
}

Window* Parser::parse_window() {
    const SourceLocation loc = loc_of(cur());
    expect(TokenType::kLParen, "'(' after OVER");
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
    return make<Window>(loc, make_list(partition_by), make_list(order_by), frame);
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

// Queries and statements.

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
    if (match(TokenType::kKwLimit)) {
        if (!match(TokenType::kKwAll)) {
            limit = parse_expr();
        }
    }
    return make<Query>(loc, with, body, make_list(order_by), offset, limit);
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
        const bool all = match(TokenType::kKwAll);
        if (!all) {
            match(TokenType::kKwDistinct);
        }
        left = make<SetOperation>(loc, op, left, parse_intersect(), all);
    }
    return left;
}

Node* Parser::parse_intersect() {
    Node* left = parse_query_term();
    while (at(TokenType::kKwIntersect)) {
        const SourceLocation loc = loc_of(advance());
        const bool all = match(TokenType::kKwAll);
        if (!all) {
            match(TokenType::kKwDistinct);
        }
        left = make<SetOperation>(loc, SetOp::kIntersect, left, parse_query_term(), all);
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
    if (!match(TokenType::kKwAll)) {
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
    if (match(TokenType::kKwGroup)) {
        expect(TokenType::kKwBy, "BY after GROUP");
        group_by.push_back(parse_group_by_item());
        while (match(TokenType::kComma)) {
            group_by.push_back(parse_group_by_item());
        }
    }
    Expression* having = nullptr;
    if (match(TokenType::kKwHaving)) {
        having = parse_expr();
    }
    return make<QuerySpecification>(
        loc, distinct, make_list(items), make_list(from), where, having, make_list(group_by));
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
        return make<AllColumns>(loc, AstList<NamePart>{});
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
            return make<AllColumns>(loc, make_list(parts));
        }
        pos_ = saved;
    }

    Expression* expr = parse_expr();
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
        return make<FunctionCall>(
            loc, single_name(token.text(source_)), false, false, make_list(args), nullptr);
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

// Relations.

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
    } else if (at_name()) {
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
