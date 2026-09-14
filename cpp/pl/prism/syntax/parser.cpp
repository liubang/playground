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

// Parser core: token navigation, error handling, AST construction helpers and
// name parsing. Expression parsing lives in parser_expression.cpp, query and
// relation parsing in parser_query.cpp.

#include "cpp/pl/prism/syntax/parser.h"

#include <utility>

#include "cpp/pl/prism/syntax/lexer.h"

namespace pl::prism::syntax {

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

} // namespace pl::prism::syntax
