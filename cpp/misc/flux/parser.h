// Copyright (c) 2023 The Authors. All rights reserved.
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

#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <utility>

#include "ast.h"
#include "scanner.h"
#include "strconv.h"
#include "token.h"

namespace pl {

struct TokenError {
    TokenError() = default;
    TokenError(std::unique_ptr<Token> t) : token(std::move(t)) {}
    std::unique_ptr<Token> token;
};

class Parser {
public:
    Parser(const std::string& input)
        : scanner_(std::make_unique<Scanner>(input.data(), input.size())), source_(input) {}

    // Parses a file of Flux source code, returning a Package
    std::unique_ptr<Package> parse_single_package(const std::string& pkgpath,
                                                  const std::string& fname);

    // Parses a file of Flux source code, returning a File
    std::unique_ptr<File> parse_file(const std::string& fname);

private:
    constexpr static char METADATA[] = "parser-type=rust";
    constexpr static uint32_t MAX_DEPTH = 80;

    // scan will read the next token from the Scanner. If peek has been used,
    // this will return the peeked token and consume it.
    std::unique_ptr<Token> scan();

    // peek will read the next token from the Scanner and then buffer it.
    // It will return information about the token.
    const Token* peek();

    // peek_with_regex is the same as peek, except that the scan step will allow scanning regexp
    // tokens.
    const Token* peek_with_regex();

    // consume will consume a token that has been retrieve using peek.
    // This will panic if a token has not been buffered with peek.
    std::unique_ptr<Token> consume();

    // expect will check if the next token is `exp` and error if it is not in either case the token
    // is consumed and returned
    std::unique_ptr<Token> expect(TokenType exp) { return expect_one_of(std::set<TokenType>{exp}); }
    std::unique_ptr<Token> expect_one_of(const std::set<TokenType>& exp);
    // If `exp` is not the next token this will record an error and continue without consuming the
    // token so that the next step in the parse may use it
    std::unique_ptr<Token> expect_or_skip(TokenType exp);

    // open will open a new block. It will expect that the next token is the sater token and mark
    // that we expect the end token in the future.
    std::unique_ptr<Token> open(TokenType start, TokenType end);

    // more will check if we should continue reading tokens for the current block. This is true when
    // the next token is not EOF and the next token is also not one that would close a block.
    bool more();

    // close will close a block that was opened using open.
    //
    // This function will always decrement the block count for the end token.
    //
    // If the next token is the end token, then this will consume the token and return the pos and
    // lit for the token. Otherwise, it will return NoPos.
    std::unique_ptr<Token> close(TokenType end);

    SourceLocation source_location(const Position& start, const Position& end);

    std::tuple<std::unique_ptr<PackageClause>,
               std::optional<std::vector<std::shared_ptr<Attribute>>>>
    parse_package_clause(const std::vector<std::shared_ptr<Attribute>>& attributes);
    std::unique_ptr<ImportDeclaration> parse_import_declaration(
        const std::optional<std::vector<std::shared_ptr<Attribute>>>& attributes);
    std::tuple<std::vector<std::shared_ptr<ImportDeclaration>>,
               std::optional<std::vector<std::shared_ptr<Attribute>>>>
    parse_import_list(std::optional<std::vector<std::shared_ptr<Attribute>>> attributes);

    std::vector<std::shared_ptr<Attribute>> parse_attribute_inner_list();
    std::unique_ptr<Attribute> parse_attribute_inner();
    std::unique_ptr<Attribute> parse_attribute_rest(std::unique_ptr<Token> tok,
                                                    const std::string& name);
    std::vector<std::shared_ptr<AttributeParam>> parse_attribute_params();

    std::vector<std::shared_ptr<Property>> parse_parameter_list();
    std::vector<std::shared_ptr<Property>> parse_property_list();
    std::vector<std::shared_ptr<Property>> parse_property_list_suffix(
        std::unique_ptr<PropertyKey> key);
    std::unique_ptr<Property> parse_parameter();
    std::unique_ptr<Property> parse_string_property();
    std::unique_ptr<Property> parse_ident_property();
    std::unique_ptr<Property> parse_invalid_property();
    std::unique_ptr<Property> parse_property_suffix(std::unique_ptr<PropertyKey> key);

    std::unique_ptr<LabelLit> parse_label_literal();
    std::tuple<std::unique_ptr<DurationLit>, TokenError> parse_duration_literal();
    std::tuple<std::unique_ptr<DateTimeLit>, TokenError> parse_time_literal();

    bool parse_pipe_operator();
    std::optional<Operator> parse_exponent_operator();
    std::optional<Operator> parse_multiplicative_operator();
    std::optional<Operator> parse_logical_unary_operator();
    std::optional<Operator> parse_comparison_operator();
    std::optional<Operator> parse_additive_operator();
    std::optional<LogicalOperator> parse_or_operator();
    std::optional<LogicalOperator> parse_and_operator();

    std::unique_ptr<PipeLit> parse_pipe_literal();
    std::unique_ptr<RegexpLit> parse_regexp_literral();
    std::unique_ptr<StringLit> parse_string_literal();
    std::unique_ptr<StringLit> new_string_literal(std::unique_ptr<Token> t);
    std::unique_ptr<Identifier> parse_identifier();
    std::unique_ptr<IntegerLit> parse_int_literal();
    std::tuple<std::unique_ptr<FloatLit>, TokenError> parse_float_literal();

    //// expression
    std::tuple<std::unique_ptr<StringExpr>, TokenError> parse_string_expression();
    std::unique_ptr<ObjectExpr> parse_object_literal();
    std::unique_ptr<ObjectExpr> parse_object_body();
    std::unique_ptr<ObjectExpr> parse_object_body_suffix(std::unique_ptr<Identifier> id);

    std::unique_ptr<Block> parse_block();
    std::unique_ptr<Expression> parse_expression();
    std::unique_ptr<Expression> parse_primary_expression();
    std::unique_ptr<Expression> parse_function_body_expression(
        std::unique_ptr<Token> lparen,
        std::unique_ptr<Token> rparen,
        std::unique_ptr<Token> arrow,
        const std::vector<std::shared_ptr<Property>>& params);
    std::unique_ptr<Expression> parse_paren_body_expression(std::unique_ptr<Token> lparen);
    std::unique_ptr<Expression> parse_paren_expression();
    std::unique_ptr<Expression> parse_function_expression(
        std::unique_ptr<Token> lparen,
        std::unique_ptr<Token> rparen,
        const std::vector<std::shared_ptr<Property>>& params);

    std::unique_ptr<Expression> parse_paren_ident_expression(std::unique_ptr<Token> lparen,
                                                             std::unique_ptr<Identifier> key);
    std::unique_ptr<Expression> parse_expression_while_more(std::unique_ptr<Expression> init,
                                                            const std::set<TokenType>& stop_tokens);
    std::unique_ptr<Expression> parse_property_value();
    std::unique_ptr<Expression> parse_array_or_dict(std::unique_ptr<Token> start);
    std::unique_ptr<Expression> parse_array_items_rest(std::unique_ptr<Token> start,
                                                       std::unique_ptr<Expression> init);
    std::unique_ptr<Expression> parse_dict_items_rest(std::unique_ptr<Token> start,
                                                      std::unique_ptr<Expression> key,
                                                      std::unique_ptr<Expression> val);
    std::unique_ptr<Expression> parse_additive_expression();
    std::unique_ptr<Expression> parse_additive_expression_suffix(std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_comparison_expression_suffix(
        std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_logical_and_expression();
    std::unique_ptr<Expression> parse_logical_or_expression_suffix(
        std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_logical_or_expression();
    std::unique_ptr<Expression> create_bad_expression(std::unique_ptr<Token> tok);
    std::unique_ptr<Expression> create_bad_expression_with_text(std::unique_ptr<Token> tok,
                                                                std::string_view text);
    std::unique_ptr<Expression> parse_conditional_expression();
    std::unique_ptr<Expression> create_placeholder_expression(const Token* tok);
    std::unique_ptr<Expression> parse_logical_and_expression_suffix(
        std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_logical_unary_expression();
    std::unique_ptr<Expression> parse_comparison_expression();
    std::unique_ptr<Expression> parse_assign_statement();
    std::unique_ptr<Expression> parse_dot_expression(std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_call_expression(std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_index_expression(std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_postfix_operator(std::unique_ptr<Expression> expr, bool* ret);
    std::unique_ptr<Expression> parse_postfix_operator_suffix(std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_postfix_expression();
    std::unique_ptr<Expression> parse_unary_expression();
    std::unique_ptr<Expression> parse_pipe_expression();
    std::unique_ptr<Expression> parse_exponent_expression();
    std::unique_ptr<Expression> parse_multiplicative_expression();
    std::unique_ptr<Expression> parse_pipe_expression_suffix(std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_exponent_expression_suffix(std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_multiplicative_expression_suffix(
        std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parse_expression_suffix(std::unique_ptr<Expression> expr);

    //// assign
    std::unique_ptr<Assignment> parse_option_assignment_suffix(std::unique_ptr<Identifier> id);

    //// statement
    std::unique_ptr<Statement> parse_expression_statement();
    std::unique_ptr<Statement> parse_ident_statement();
    std::unique_ptr<Statement> parse_option_assignment();
    std::unique_ptr<Statement> parse_builtin_statement();
    std::unique_ptr<Statement> parse_testcase_statement();
    std::unique_ptr<Statement> parse_return_statement();
    std::unique_ptr<Statement> parse_statement_inner(
        const std::optional<std::vector<std::shared_ptr<Attribute>>>& attributes);
    std::unique_ptr<Statement> parse_statement(
        const std::optional<std::vector<std::shared_ptr<Attribute>>>& attributes);
    std::vector<std::shared_ptr<Statement>> parse_statement_list(
        const std::optional<std::vector<std::shared_ptr<Attribute>>>& attributes);

    //// type
    std::unique_ptr<MonoType> parse_monotype();
    std::unique_ptr<TypeExpression> parse_type_expression();
    std::vector<std::shared_ptr<TypeConstraint>> parse_constraints();
    std::unique_ptr<TypeConstraint> parse_constraint();
    std::unique_ptr<MonoType> parse_record_type();
    std::unique_ptr<MonoType> parse_function_type();
    std::unique_ptr<MonoType> parse_dynamic_type();
    std::unique_ptr<MonoType> parse_basic_type();
    std::unique_ptr<MonoType> parse_tvar();

    template <typename T> std::optional<T> depth_guard(std::function<T()> fn) {
        ++depth_;
        if (depth_ > MAX_DEPTH) {
            errs_.emplace_back("Program is nested too deep");
            return std::nullopt;
        }
        T ret = fn();
        --depth_;
        return std::move(ret);
    }

private:
    std::unique_ptr<Scanner> scanner_;
    std::unique_ptr<Token> token_;
    std::vector<std::string> errs_;
    std::map<TokenType, int32_t> blocks_;
    std::string source_;
    std::string fname_;
    uint32_t depth_{0};
};

} // namespace pl
