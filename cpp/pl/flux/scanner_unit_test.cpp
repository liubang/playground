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

#include "cpp/pl/flux/scanner.h"

#include <gtest/gtest.h>

namespace pl {
namespace {

TEST(FluxScannerTest, ScansCommentsIdentifiersAndLiterals) {
    const std::string source = R"(
        // lead comment
        builtin foo = "bar"
        42u 1h 2024-01-02T03:04:05Z
    )";

    Scanner scanner(source.data(), source.size());

    auto builtin = scanner.scan();
    ASSERT_NE(builtin, nullptr);
    EXPECT_EQ(TokenType::Builtin, builtin->tok);
    ASSERT_EQ(1, builtin->comments.size());
    EXPECT_EQ("// lead comment\n", builtin->comments[0]->text);

    auto ident = scanner.scan();
    ASSERT_NE(ident, nullptr);
    EXPECT_EQ(TokenType::Ident, ident->tok);
    EXPECT_EQ("foo", ident->lit);

    auto assign = scanner.scan();
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(TokenType::Assign, assign->tok);

    auto string = scanner.scan();
    ASSERT_NE(string, nullptr);
    EXPECT_EQ(TokenType::String, string->tok);
    EXPECT_EQ("\"bar\"", string->lit);

    auto uint = scanner.scan();
    ASSERT_NE(uint, nullptr);
    EXPECT_EQ(TokenType::UInt, uint->tok);
    EXPECT_EQ("42u", uint->lit);

    auto duration = scanner.scan();
    ASSERT_NE(duration, nullptr);
    EXPECT_EQ(TokenType::Duration, duration->tok);
    EXPECT_EQ("1h", duration->lit);

    auto time = scanner.scan();
    ASSERT_NE(time, nullptr);
    EXPECT_EQ(TokenType::Time, time->tok);
    EXPECT_EQ("2024-01-02T03:04:05Z", time->lit);
}

TEST(FluxScannerTest, ScanWithRegexDistinguishesRegexFromDivision) {
    const std::string regex_source = "/cpu.*/";
    Scanner regex_scanner(regex_source.data(), regex_source.size());
    auto regex = regex_scanner.scan_with_regex();
    ASSERT_NE(regex, nullptr);
    EXPECT_EQ(TokenType::Regex, regex->tok);
    EXPECT_EQ("/cpu.*/", regex->lit);

    const std::string div_source = "a / b";
    Scanner div_scanner(div_source.data(), div_source.size());
    auto ident = div_scanner.scan();
    ASSERT_NE(ident, nullptr);
    EXPECT_EQ(TokenType::Ident, ident->tok);
    auto divide = div_scanner.scan();
    ASSERT_NE(divide, nullptr);
    EXPECT_EQ(TokenType::Div, divide->tok);
    auto rhs = div_scanner.scan();
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(TokenType::Ident, rhs->tok);
}

TEST(FluxScannerTest, UnreadRestoresPreviousTokenBoundary) {
    const std::string source = "value";
    Scanner scanner(source.data(), source.size());

    auto first = scanner.scan();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(TokenType::Ident, first->tok);
    EXPECT_EQ("value", first->lit);

    scanner.unread();

    auto second = scanner.scan();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(TokenType::Ident, second->tok);
    EXPECT_EQ("value", second->lit);
    EXPECT_EQ(first->start_offset, second->start_offset);
    EXPECT_EQ(first->end_offset, second->end_offset);
}

TEST(FluxScannerTest, ReturnsIllegalTokenForUnexpectedCharacters) {
    const std::string source = ";";
    Scanner scanner(source.data(), source.size());

    auto illegal = scanner.scan();
    ASSERT_NE(illegal, nullptr);
    EXPECT_EQ(TokenType::Illegal, illegal->tok);
    EXPECT_EQ(";", illegal->lit);
    EXPECT_EQ(1u, illegal->start_pos.line);
    EXPECT_EQ(1u, illegal->start_pos.column);
    EXPECT_EQ(0u, illegal->start_offset);
    EXPECT_EQ(1u, illegal->end_offset);
    EXPECT_EQ(1u, illegal->end_pos.line);
    EXPECT_EQ(2u, illegal->end_pos.column);

    auto eof = scanner.scan();
    ASSERT_NE(eof, nullptr);
    EXPECT_EQ(TokenType::Eof, eof->tok);
}

} // namespace
} // namespace pl
