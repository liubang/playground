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

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/arena/arena.h"
#include "cpp/pl/prism/syntax/ast.h"
#include "cpp/pl/prism/syntax/token.h"

namespace pl::prism::syntax {

struct ParseError {
    uint32_t offset;
    uint32_t line;
    uint32_t column;
    std::string message;
};

struct ParseResult {
    Node* root = nullptr;
    std::vector<ParseError> errors;

    [[nodiscard]] bool ok() const { return errors.empty(); }
};

// ASCII case-insensitive comparison used for soft keywords and type names.
inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool iequals(std::string_view a, std::string_view b) {
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

// Recursive-descent parser for Trino SQL (P0 scope: expressions + SELECT core
// including JOIN/CTE/window/UNNEST/subqueries/set operations).
//
// The AST is arena-allocated and stays valid as long as the Parser object and
// the source text are alive.
class Parser {
public:
    explicit Parser(std::string_view source);

    // Parses a single statement (a Query in P0 scope), optionally followed by
    // a semicolon.
    ParseResult parse_statement();

    // Parses a single expression; useful for tests and tooling.
    ParseResult parse_expression();

private:
    // Token navigation.
    [[nodiscard]] const Token& cur() const { return tokens_[pos_]; }
    [[nodiscard]] const Token& peek(uint32_t n) const;
    const Token& advance();
    [[nodiscard]] bool at(TokenType type) const { return cur().type == type; }
    bool match(TokenType type);
    const Token& expect(TokenType type, const char* what);
    // Soft (non-reserved) keywords: matched as identifiers by text.
    [[nodiscard]] bool at_soft(std::string_view word) const;
    bool match_soft(std::string_view word);
    [[nodiscard]] bool at_name() const;
    [[nodiscard]] std::string_view cur_text() const;
    [[nodiscard]] SourceLocation loc_of(const Token& token) const;
    // True if, skipping n leading '(' tokens, a query keyword follows.
    [[nodiscard]] bool is_query_start(uint32_t n) const;

    [[noreturn]] void fail(const Token& token, std::string message);

    // AST construction helpers.
    template <typename T, typename... Args> T* make(SourceLocation loc, Args&&... args) {
        return arena_.template allocate_object<T>(loc, std::forward<Args>(args)...);
    }
    template <typename T> AstList<T> make_list(const std::vector<T>& items) {
        AstList<T> out;
        out.size = static_cast<uint32_t>(items.size());
        if (!items.empty()) {
            out.data = static_cast<T*>(arena_.allocate(sizeof(T) * items.size(), alignof(T)));
            std::copy(items.begin(), items.end(), out.data);
        }
        return out;
    }
    AstList<NamePart> single_name(std::string_view text);

    // Names.
    NamePart parse_name_part();
    AstList<NamePart> parse_qualified_name();

    // Expressions.
    Expression* parse_expr();
    Expression* parse_or();
    Expression* parse_and();
    Expression* parse_not();
    Expression* parse_predicate();
    Expression* parse_concat();
    Expression* parse_additive();
    Expression* parse_multiplicative();
    Expression* parse_unary();
    Expression* parse_postfix();
    Expression* parse_primary();
    Expression* parse_paren(SourceLocation loc);
    Expression* parse_named_primary(SourceLocation loc);
    Expression* parse_case(SourceLocation loc);
    Expression* parse_cast(SourceLocation loc, bool try_cast);
    Expression* parse_interval(SourceLocation loc);
    Expression* parse_function_call(SourceLocation loc, AstList<NamePart> name);
    Expression* finish_function_call(SourceLocation loc,
                                     AstList<NamePart> name,
                                     bool distinct,
                                     bool wildcard,
                                     std::vector<Expression*> args,
                                     std::vector<SortItem*> order_by);
    Expression* parse_trim(SourceLocation loc);
    Expression* parse_substring_special(SourceLocation loc, std::string_view word);
    Expression* parse_position_special(SourceLocation loc);
    Expression* parse_overlay_special(SourceLocation loc);
    TypeName* parse_type();
    Window* parse_window();
    WindowFrame* parse_frame();
    FrameBound parse_frame_bound();
    [[nodiscard]] bool looks_like_lambda() const;

    // Queries and statements.
    Query* parse_query();
    Node* parse_explain();
    With* parse_with();
    Node* parse_set_operation();
    Node* parse_intersect();
    Node* parse_query_term();
    Node* parse_query_specification();
    Node* parse_values();
    SelectItem* parse_select_item();
    Expression* parse_group_by_item();
    void parse_corresponding(bool& corresponding, std::vector<NamePart>& corresponding_by);
    std::vector<SortItem*> parse_sort_list();

    // Relations.
    Relation* parse_relation();
    Relation* parse_relation_primary();
    Relation* maybe_alias(Relation* relation);

    struct ParseAbort {};

    std::string_view source_;
    std::vector<Token> tokens_;
    uint32_t pos_ = 0;
    Arena arena_;
    std::vector<ParseError> errors_;
};

} // namespace pl::prism::syntax
