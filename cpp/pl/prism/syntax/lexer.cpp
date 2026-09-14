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

#include "cpp/pl/prism/syntax/lexer.h"

#include <algorithm>
#include <array>

namespace pl::prism::syntax {

namespace {

constexpr bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

constexpr bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

constexpr bool is_ident_part(char c) {
    return is_ident_start(c) || is_digit(c) || c == '$';
}

constexpr char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

constexpr bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

struct KeywordEntry {
    std::string_view text;
    TokenType type;
};

// Reserved keywords of the Trino 476 grammar, sorted by text. Non-reserved
// keywords are intentionally absent: they lex as kIdentifier and are
// disambiguated contextually by the parser.
// The longest entry, CURRENT_TIMESTAMP, is 17 characters.
constexpr std::array kKeywords = std::to_array<KeywordEntry>({
    {"ALTER", TokenType::kKwAlter},
    {"ANALYZE", TokenType::kKwAnalyze},
    {"AND", TokenType::kKwAnd},
    {"ARRAY", TokenType::kKwArray},
    {"AS", TokenType::kKwAs},
    {"AT", TokenType::kKwAt},
    {"BETWEEN", TokenType::kKwBetween},
    {"BY", TokenType::kKwBy},
    {"CASE", TokenType::kKwCase},
    {"CAST", TokenType::kKwCast},
    {"CONSTRAINT", TokenType::kKwConstraint},
    {"CREATE", TokenType::kKwCreate},
    {"CROSS", TokenType::kKwCross},
    {"CUBE", TokenType::kKwCube},
    {"CURRENT", TokenType::kKwCurrent},
    {"CURRENT_CATALOG", TokenType::kKwCurrentCatalog},
    {"CURRENT_DATE", TokenType::kKwCurrentDate},
    {"CURRENT_PATH", TokenType::kKwCurrentPath},
    {"CURRENT_ROLE", TokenType::kKwCurrentRole},
    {"CURRENT_SCHEMA", TokenType::kKwCurrentSchema},
    {"CURRENT_TIME", TokenType::kKwCurrentTime},
    {"CURRENT_TIMESTAMP", TokenType::kKwCurrentTimestamp},
    {"CURRENT_USER", TokenType::kKwCurrentUser},
    {"DEALLOCATE", TokenType::kKwDeallocate},
    {"DELETE", TokenType::kKwDelete},
    {"DESCRIBE", TokenType::kKwDescribe},
    {"DISTINCT", TokenType::kKwDistinct},
    {"DROP", TokenType::kKwDrop},
    {"ELSE", TokenType::kKwElse},
    {"END", TokenType::kKwEnd},
    {"ESCAPE", TokenType::kKwEscape},
    {"EXCEPT", TokenType::kKwExcept},
    {"EXECUTE", TokenType::kKwExecute},
    {"EXISTS", TokenType::kKwExists},
    {"EXPLAIN", TokenType::kKwExplain},
    {"EXTRACT", TokenType::kKwExtract},
    {"FALSE", TokenType::kKwFalse},
    {"FOR", TokenType::kKwFor},
    {"FROM", TokenType::kKwFrom},
    {"FULL", TokenType::kKwFull},
    {"GROUP", TokenType::kKwGroup},
    {"GROUPING", TokenType::kKwGrouping},
    {"HAVING", TokenType::kKwHaving},
    {"IN", TokenType::kKwIn},
    {"INNER", TokenType::kKwInner},
    {"INSERT", TokenType::kKwInsert},
    {"INTERSECT", TokenType::kKwIntersect},
    {"INTERVAL", TokenType::kKwInterval},
    {"INTO", TokenType::kKwInto},
    {"IS", TokenType::kKwIs},
    {"JOIN", TokenType::kKwJoin},
    {"JSON_ARRAY", TokenType::kKwJsonArray},
    {"JSON_EXISTS", TokenType::kKwJsonExists},
    {"JSON_OBJECT", TokenType::kKwJsonObject},
    {"JSON_QUERY", TokenType::kKwJsonQuery},
    {"JSON_TABLE", TokenType::kKwJsonTable},
    {"JSON_VALUE", TokenType::kKwJsonValue},
    {"LEFT", TokenType::kKwLeft},
    {"LIKE", TokenType::kKwLike},
    {"LIMIT", TokenType::kKwLimit},
    {"LISTAGG", TokenType::kKwListagg},
    {"LOCALTIME", TokenType::kKwLocaltime},
    {"LOCALTIMESTAMP", TokenType::kKwLocaltimestamp},
    {"NATURAL", TokenType::kKwNatural},
    {"NORMALIZE", TokenType::kKwNormalize},
    {"NOT", TokenType::kKwNot},
    {"NULL", TokenType::kKwNull},
    {"ON", TokenType::kKwOn},
    {"OR", TokenType::kKwOr},
    {"ORDER", TokenType::kKwOrder},
    {"OUTER", TokenType::kKwOuter},
    {"PREPARE", TokenType::kKwPrepare},
    {"RECURSIVE", TokenType::kKwRecursive},
    {"RIGHT", TokenType::kKwRight},
    {"ROLLUP", TokenType::kKwRollup},
    {"SELECT", TokenType::kKwSelect},
    {"TABLE", TokenType::kKwTable},
    {"THEN", TokenType::kKwThen},
    {"TO", TokenType::kKwTo},
    {"TRUE", TokenType::kKwTrue},
    {"UESCAPE", TokenType::kKwUescape},
    {"UNION", TokenType::kKwUnion},
    {"UNNEST", TokenType::kKwUnnest},
    {"USING", TokenType::kKwUsing},
    {"VALUES", TokenType::kKwValues},
    {"WHEN", TokenType::kKwWhen},
    {"WHERE", TokenType::kKwWhere},
    {"WITH", TokenType::kKwWith},
});

static_assert(std::is_sorted(kKeywords.begin(),
                             kKeywords.end(),
                             [](const KeywordEntry& a, const KeywordEntry& b) {
                                 return a.text < b.text;
                             }));

constexpr size_t kMaxKeywordLength = 17; // CURRENT_TIMESTAMP

TokenType lookup_keyword(std::string_view ident) {
    if (ident.size() > kMaxKeywordLength) {
        return TokenType::kIdentifier;
    }
    std::array<char, kMaxKeywordLength> buf{};
    for (size_t i = 0; i < ident.size(); ++i) {
        buf[i] = ascii_upper(ident[i]);
    }
    const std::string_view key(buf.data(), ident.size());
    const auto* it = std::lower_bound(
        kKeywords.begin(), kKeywords.end(), key, [](const KeywordEntry& e, std::string_view v) {
            return e.text < v;
        });
    if (it != kKeywords.end() && it->text == key) {
        return it->type;
    }
    return TokenType::kIdentifier;
}

} // namespace

char Lexer::peek(uint32_t lookahead) const {
    const uint32_t idx = pos_ + lookahead;
    return idx < source_.size() ? source_[idx] : '\0';
}

char Lexer::advance() {
    const char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

Token Lexer::make_token(TokenType type, uint32_t start, uint32_t line, uint32_t column) const {
    return Token{type, start, pos_ - start, line, column};
}

void Lexer::report(uint32_t offset, uint32_t line, uint32_t column, std::string message) {
    errors_.push_back(LexError{offset, line, column, std::move(message)});
}

void Lexer::skip_trivia() {
    for (;;) {
        const char c = peek();
        if (is_whitespace(c)) {
            advance();
            continue;
        }
        if (c == '-' && peek(1) == '-') {
            while (!eof() && peek() != '\n') {
                advance();
            }
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            const uint32_t start = pos_;
            const uint32_t line = line_;
            const uint32_t column = column_;
            advance();
            advance();
            bool closed = false;
            while (!eof()) {
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    closed = true;
                    break;
                }
                advance();
            }
            if (!closed) {
                report(start, line, column, "unterminated block comment");
            }
            continue;
        }
        break;
    }
}

Token Lexer::next_token() {
    skip_trivia();

    const uint32_t start = pos_;
    const uint32_t line = line_;
    const uint32_t column = column_;

    if (eof()) {
        return make_token(TokenType::kEof, start, line, column);
    }

    const char c = peek();

    if (is_ident_start(c)) {
        // Prefixed literals: X'01' (binary), U&'str' (unicode string).
        if ((c == 'X' || c == 'x') && peek(1) == '\'') {
            return lex_string(TokenType::kBinaryString, 1);
        }
        if ((c == 'U' || c == 'u') && peek(1) == '&' && peek(2) == '\'') {
            return lex_string(TokenType::kUnicodeString, 2);
        }
        return lex_identifier_or_keyword();
    }

    if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
        return lex_number();
    }

    switch (c) {
        case '\'':
            return lex_string(TokenType::kString, 0);
        case '"':
            return lex_quoted_identifier();
        case '(':
            advance();
            return make_token(TokenType::kLParen, start, line, column);
        case ')':
            advance();
            return make_token(TokenType::kRParen, start, line, column);
        case '[':
            advance();
            return make_token(TokenType::kLBracket, start, line, column);
        case ']':
            advance();
            return make_token(TokenType::kRBracket, start, line, column);
        case ',':
            advance();
            return make_token(TokenType::kComma, start, line, column);
        case ';':
            advance();
            return make_token(TokenType::kSemicolon, start, line, column);
        case '.':
            advance();
            return make_token(TokenType::kDot, start, line, column);
        case ':':
            advance();
            if (peek() == ':') {
                advance();
                return make_token(TokenType::kDoubleColon, start, line, column);
            }
            return make_token(TokenType::kColon, start, line, column);
        case '+':
            advance();
            return make_token(TokenType::kPlus, start, line, column);
        case '-':
            advance();
            if (peek() == '>') {
                advance();
                return make_token(TokenType::kArrow, start, line, column);
            }
            return make_token(TokenType::kMinus, start, line, column);
        case '*':
            advance();
            return make_token(TokenType::kStar, start, line, column);
        case '/':
            advance();
            return make_token(TokenType::kSlash, start, line, column);
        case '%':
            advance();
            return make_token(TokenType::kPercent, start, line, column);
        case '?':
            advance();
            return make_token(TokenType::kQuestion, start, line, column);
        case '=':
            advance();
            if (peek() == '>') {
                advance();
                return make_token(TokenType::kFatArrow, start, line, column);
            }
            return make_token(TokenType::kEq, start, line, column);
        case '!':
            advance();
            if (peek() == '=') {
                advance();
                return make_token(TokenType::kNeq, start, line, column);
            }
            report(start, line, column, "unexpected character '!', did you mean '!='?");
            return make_token(TokenType::kIllegal, start, line, column);
        case '<':
            advance();
            if (peek() == '=') {
                advance();
                return make_token(TokenType::kLte, start, line, column);
            }
            if (peek() == '>') {
                advance();
                return make_token(TokenType::kNeq, start, line, column);
            }
            return make_token(TokenType::kLt, start, line, column);
        case '>':
            advance();
            if (peek() == '=') {
                advance();
                return make_token(TokenType::kGte, start, line, column);
            }
            return make_token(TokenType::kGt, start, line, column);
        case '|':
            advance();
            if (peek() == '|') {
                advance();
                return make_token(TokenType::kConcat, start, line, column);
            }
            report(start, line, column, "unexpected character '|', did you mean '||'?");
            return make_token(TokenType::kIllegal, start, line, column);
        default:
            advance();
            report(start, line, column, std::string("unexpected character '") + c + "'");
            return make_token(TokenType::kIllegal, start, line, column);
    }
}

Token Lexer::lex_identifier_or_keyword() {
    const uint32_t start = pos_;
    const uint32_t line = line_;
    const uint32_t column = column_;
    while (!eof() && is_ident_part(peek())) {
        advance();
    }
    const TokenType type = lookup_keyword(source_.substr(start, pos_ - start));
    return make_token(type, start, line, column);
}

Token Lexer::lex_number() {
    const uint32_t start = pos_;
    const uint32_t line = line_;
    const uint32_t column = column_;

    // Consumes a run of digits with optional single '_' separators between
    // digits (Trino 445+): 1_000 is one token, 1__0 and 1_ are not.
    const auto consume_digits = [this](bool (*pred)(char)) {
        while (!eof() && (pred(peek()) || (peek() == '_' && pred(peek(1))))) {
            advance();
        }
    };

    // Base-prefixed literals: 0x..., 0o..., 0b...
    if (peek() == '0' && pos_ + 1 < source_.size()) {
        const char kind = peek(1);
        if (kind == 'x' || kind == 'X' || kind == 'o' || kind == 'O' || kind == 'b' ||
            kind == 'B') {
            advance();
            advance();
            switch (kind) {
                case 'x':
                case 'X':
                    consume_digits([](char c) {
                        return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                    });
                    break;
                case 'o':
                case 'O':
                    consume_digits([](char c) { return c >= '0' && c <= '7'; });
                    break;
                default:
                    consume_digits([](char c) { return c == '0' || c == '1'; });
                    break;
            }
            if (pos_ - start == 2) {
                report(start, line, column, "expected digits after base prefix");
                return make_token(TokenType::kIllegal, start, line, column);
            }
            return make_token(TokenType::kNumber, start, line, column);
        }
    }

    if (peek() == '.') {
        advance();
        consume_digits(is_digit);
    } else {
        consume_digits(is_digit);
        if (peek() == '.') {
            advance();
            consume_digits(is_digit);
        }
    }

    // Exponent: only consumed when it forms a valid suffix, so that "1e" lexes
    // as number("1") followed by identifier("e").
    if (peek() == 'e' || peek() == 'E') {
        if (is_digit(peek(1))) {
            advance();
            consume_digits(is_digit);
        } else if ((peek(1) == '+' || peek(1) == '-') && is_digit(peek(2))) {
            advance();
            advance();
            consume_digits(is_digit);
        }
    }

    return make_token(TokenType::kNumber, start, line, column);
}

Token Lexer::lex_string(TokenType type, uint32_t prefix_length) {
    const uint32_t start = pos_;
    const uint32_t line = line_;
    const uint32_t column = column_;

    for (uint32_t i = 0; i < prefix_length; ++i) {
        advance();
    }
    advance(); // opening quote

    while (!eof()) {
        if (advance() == '\'') {
            if (peek() == '\'') { // doubled quote escape
                advance();
                continue;
            }
            return make_token(type, start, line, column);
        }
    }

    report(start, line, column, "unterminated string literal");
    return make_token(TokenType::kIllegal, start, line, column);
}

Token Lexer::lex_quoted_identifier() {
    const uint32_t start = pos_;
    const uint32_t line = line_;
    const uint32_t column = column_;

    advance(); // opening quote
    while (!eof()) {
        if (advance() == '"') {
            if (peek() == '"') { // doubled quote escape
                advance();
                continue;
            }
            return make_token(TokenType::kQuotedIdentifier, start, line, column);
        }
    }

    report(start, line, column, "unterminated quoted identifier");
    return make_token(TokenType::kIllegal, start, line, column);
}

} // namespace pl::prism::syntax
