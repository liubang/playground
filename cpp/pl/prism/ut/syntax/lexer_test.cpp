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

#include <string_view>
#include <vector>

#include "cpp/pl/prism/syntax/lexer.h"
#include "gtest/gtest.h"

namespace pl::prism::syntax {
namespace {

struct LexResult {
    std::vector<Token> tokens;
    std::vector<LexError> errors;
};

LexResult lex(std::string_view source) {
    Lexer lexer(source);
    LexResult result;
    for (;;) {
        Token token = lexer.next_token();
        const bool done = token.type == TokenType::kEof;
        result.tokens.push_back(token);
        if (done) {
            break;
        }
    }
    result.errors = lexer.errors();
    return result;
}

std::vector<TokenType> types(const LexResult& result) {
    std::vector<TokenType> out;
    out.reserve(result.tokens.size());
    for (const Token& token : result.tokens) {
        out.push_back(token.type);
    }
    return out;
}

TEST(LexerTest, Eof) {
    auto result = lex("");
    EXPECT_TRUE(result.errors.empty());
    ASSERT_EQ(1, result.tokens.size());
    EXPECT_EQ(TokenType::kEof, result.tokens[0].type);
}

TEST(LexerTest, SimpleSelect) {
    auto result = lex("SELECT a, b FROM t WHERE x = 1;");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(std::vector<TokenType>({TokenType::kKwSelect,
                                      TokenType::kIdentifier,
                                      TokenType::kComma,
                                      TokenType::kIdentifier,
                                      TokenType::kKwFrom,
                                      TokenType::kIdentifier,
                                      TokenType::kKwWhere,
                                      TokenType::kIdentifier,
                                      TokenType::kEq,
                                      TokenType::kNumber,
                                      TokenType::kSemicolon,
                                      TokenType::kEof}),
              types(result));
}

TEST(LexerTest, KeywordsAreCaseInsensitive) {
    auto result = lex("SeLeCt current_TIMESTAMP");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(std::vector<TokenType>(
                  {TokenType::kKwSelect, TokenType::kKwCurrentTimestamp, TokenType::kEof}),
              types(result));
    // Original text is preserved (no case folding on the token itself).
    EXPECT_EQ("SeLeCt", result.tokens[0].text("SeLeCt current_TIMESTAMP"));
}

TEST(LexerTest, NonReservedWordsAreIdentifiers) {
    auto result = lex("date first varchar value");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(std::vector<TokenType>({TokenType::kIdentifier,
                                      TokenType::kIdentifier,
                                      TokenType::kIdentifier,
                                      TokenType::kIdentifier,
                                      TokenType::kEof}),
              types(result));
}

TEST(LexerTest, IdentifierCharacters) {
    auto result = lex("a$1_2 _x very_long_identifier_that_exceeds_keyword_length");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(std::vector<TokenType>({TokenType::kIdentifier,
                                      TokenType::kIdentifier,
                                      TokenType::kIdentifier,
                                      TokenType::kEof}),
              types(result));
}

TEST(LexerTest, QuotedIdentifier) {
    constexpr std::string_view source = "\"select \"\"x\"\"\"";
    auto result = lex(source);
    EXPECT_TRUE(result.errors.empty());
    ASSERT_EQ(2, result.tokens.size());
    EXPECT_EQ(TokenType::kQuotedIdentifier, result.tokens[0].type);
    EXPECT_EQ(source, result.tokens[0].text(source));
}

TEST(LexerTest, StringWithDoubledQuote) {
    constexpr std::string_view source = "'it''s'";
    auto result = lex(source);
    EXPECT_TRUE(result.errors.empty());
    ASSERT_EQ(2, result.tokens.size());
    EXPECT_EQ(TokenType::kString, result.tokens[0].type);
    EXPECT_EQ(source, result.tokens[0].text(source));
}

TEST(LexerTest, MultilineString) {
    auto result = lex("'a\nb'");
    EXPECT_TRUE(result.errors.empty());
    ASSERT_EQ(2, result.tokens.size());
    EXPECT_EQ(TokenType::kString, result.tokens[0].type);
    EXPECT_EQ(2, result.tokens[1].line); // EOF on the second line
}

TEST(LexerTest, PrefixedStrings) {
    auto result = lex("U&'d6\\00e4' x'6564' X'FF'");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(std::vector<TokenType>({TokenType::kUnicodeString,
                                      TokenType::kBinaryString,
                                      TokenType::kBinaryString,
                                      TokenType::kEof}),
              types(result));
}

TEST(LexerTest, Numbers) {
    auto result = lex("42 1.5 .5 1. 1e10 1.5E-3 6e+2");
    EXPECT_TRUE(result.errors.empty());
    std::vector<TokenType> expected(7, TokenType::kNumber);
    expected.push_back(TokenType::kEof);
    EXPECT_EQ(expected, types(result));
}

TEST(LexerTest, NumbersWithSeparatorsAndBases) {
    auto result = lex("1_000 123_456.789_0123 1_000e1_0 0x123_abc_def 0XAB 0o17 0O7_7 0b101 0B1_0");
    EXPECT_TRUE(result.errors.empty());
    std::vector<TokenType> expected(9, TokenType::kNumber);
    expected.push_back(TokenType::kEof);
    EXPECT_EQ(expected, types(result));
}

TEST(LexerTest, BasePrefixWithoutDigitsIsIllegal) {
    auto result = lex("0x");
    ASSERT_EQ(1, result.errors.size());
    EXPECT_EQ(TokenType::kIllegal, result.tokens[0].type);
}

TEST(LexerTest, NumberFollowedByIdentifier) {
    // "1e" is not a valid exponent: lexes as number "1" then identifier "e".
    auto result = lex("1e");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(std::vector<TokenType>({TokenType::kNumber, TokenType::kIdentifier, TokenType::kEof}),
              types(result));
}

TEST(LexerTest, CommentsAreSkipped) {
    auto result = lex("SELECT -- line comment\n a /* block\n comment */ FROM t");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(std::vector<TokenType>({TokenType::kKwSelect,
                                      TokenType::kIdentifier,
                                      TokenType::kKwFrom,
                                      TokenType::kIdentifier,
                                      TokenType::kEof}),
              types(result));
}

TEST(LexerTest, CommentBetweenTokens) {
    auto result = lex("a/* c */+b-- c");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(
        std::vector<TokenType>(
            {TokenType::kIdentifier, TokenType::kPlus, TokenType::kIdentifier, TokenType::kEof}),
        types(result));
}

TEST(LexerTest, Operators) {
    auto result = lex("<> != <= >= < > = || :: -> => + - * / % ( ) [ ] , ; . :");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(std::vector<TokenType>(
                  {TokenType::kNeq,       TokenType::kNeq,      TokenType::kLte,
                   TokenType::kGte,       TokenType::kLt,       TokenType::kGt,
                   TokenType::kEq,        TokenType::kConcat,   TokenType::kDoubleColon,
                   TokenType::kArrow,     TokenType::kFatArrow, TokenType::kPlus,
                   TokenType::kMinus,     TokenType::kStar,     TokenType::kSlash,
                   TokenType::kPercent,   TokenType::kLParen,   TokenType::kRParen,
                   TokenType::kLBracket,  TokenType::kRBracket, TokenType::kComma,
                   TokenType::kSemicolon, TokenType::kDot,      TokenType::kColon,
                   TokenType::kEof}),
              types(result));
}

TEST(LexerTest, UnterminatedString) {
    auto result = lex("'abc");
    ASSERT_EQ(1, result.errors.size());
    EXPECT_EQ(TokenType::kIllegal, result.tokens[0].type);
    EXPECT_EQ(0, result.errors[0].offset);
    EXPECT_EQ(1, result.errors[0].line);
}

TEST(LexerTest, UnterminatedQuotedIdentifier) {
    auto result = lex("\"abc");
    ASSERT_EQ(1, result.errors.size());
    EXPECT_EQ(TokenType::kIllegal, result.tokens[0].type);
}

TEST(LexerTest, UnterminatedBlockComment) {
    auto result = lex("SELECT /* abc");
    ASSERT_EQ(1, result.errors.size());
    EXPECT_EQ(std::vector<TokenType>({TokenType::kKwSelect, TokenType::kEof}), types(result));
}

TEST(LexerTest, UnexpectedCharacter) {
    auto result = lex("`");
    ASSERT_EQ(1, result.errors.size());
    EXPECT_EQ(TokenType::kIllegal, result.tokens[0].type);
}

TEST(LexerTest, SinglePipeIsIllegal) {
    auto result = lex("a | b");
    ASSERT_EQ(1, result.errors.size());
    EXPECT_EQ(TokenType::kIllegal, result.tokens[1].type);
}

TEST(LexerTest, LineAndColumnTracking) {
    auto result = lex("SELECT\n  a\nFROM t");
    EXPECT_TRUE(result.errors.empty());
    const Token& a = result.tokens[1];
    EXPECT_EQ(2, a.line);
    EXPECT_EQ(3, a.column);
    EXPECT_EQ(9, a.offset);
    const Token& from = result.tokens[2];
    EXPECT_EQ(3, from.line);
    EXPECT_EQ(1, from.column);
}

TEST(LexerTest, AllReservedKeywords) {
    constexpr std::string_view source =
        "ALL ALTER ANALYZE AND ARRAY AS AT BETWEEN BY CASE CAST CONSTRAINT CREATE CROSS CUBE "
        "CURRENT CURRENT_CATALOG CURRENT_DATE CURRENT_PATH CURRENT_ROLE CURRENT_SCHEMA "
        "CURRENT_TIME CURRENT_TIMESTAMP CURRENT_USER DEALLOCATE DELETE DESCRIBE "
        "DISTINCT DROP ELSE END ESCAPE EXCEPT EXECUTE EXISTS EXPLAIN EXTRACT FALSE FOR "
        "FROM FULL GROUP GROUPING HAVING IN INNER INSERT INTERSECT INTERVAL INTO IS JOIN "
        "JSON_ARRAY JSON_EXISTS JSON_OBJECT JSON_QUERY JSON_TABLE JSON_VALUE "
        "LEFT LIKE LIMIT LISTAGG LOCALTIME LOCALTIMESTAMP NATURAL NORMALIZE NOT NULL "
        "ON OR ORDER OUTER PREPARE RECURSIVE RIGHT ROLLUP SELECT TABLE THEN TO "
        "TRUE UESCAPE UNION UNNEST USING VALUES WHEN WHERE WITH";
    auto result = lex(source);
    EXPECT_TRUE(result.errors.empty());
    ASSERT_EQ(90, result.tokens.size()); // 89 keywords + EOF
    for (size_t i = 0; i + 1 < result.tokens.size(); ++i) {
        EXPECT_NE(TokenType::kIdentifier, result.tokens[i].type)
            << "keyword not recognized: " << result.tokens[i].text(source);
    }
    EXPECT_EQ(TokenType::kEof, result.tokens.back().type);
}

} // namespace
} // namespace pl::prism::syntax
