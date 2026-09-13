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

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/prism/syntax/token.h"

namespace pl::prism::syntax {

struct LexError {
    uint32_t offset;
    uint32_t line;
    uint32_t column;
    std::string message;
};

// Hand-written table-driven lexer for Trino SQL. Zero-copy: tokens reference
// the source text, so the source must outlive the lexer and its tokens.
class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    // Returns the next token. Skips whitespace and comments. On a lexical
    // error, records a LexError and returns a kIllegal token spanning the
    // offending input, then continues from a recoverable position.
    Token next_token();

    [[nodiscard]] const std::vector<LexError>& errors() const { return errors_; }
    [[nodiscard]] std::string_view source() const { return source_; }

private:
    [[nodiscard]] bool eof() const { return pos_ >= source_.size(); }
    [[nodiscard]] char peek(uint32_t lookahead = 0) const;
    char advance();

    [[nodiscard]] Token make_token(TokenType type,
                                   uint32_t start,
                                   uint32_t line,
                                   uint32_t column) const;
    void report(uint32_t offset, uint32_t line, uint32_t column, std::string message);

    void skip_trivia();
    Token lex_identifier_or_keyword();
    Token lex_number();
    // Scans a quoted literal starting at the current position. prefix_length is
    // the number of characters before the opening quote (0 for 'str', 1 for
    // X'01', 2 for U&'str').
    Token lex_string(TokenType type, uint32_t prefix_length);
    Token lex_quoted_identifier();

    std::string_view source_;
    uint32_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t column_ = 1;
    std::vector<LexError> errors_;
};

} // namespace pl::prism::syntax
