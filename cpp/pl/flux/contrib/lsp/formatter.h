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
// Created: 2026/05/18 00:46

#pragma once

#include <string>

#include "cpp/pl/flux/syntax/ast.h"

namespace pl::flux::lsp {

struct FormatOptions {
    int indent_width = 4;
    bool use_tabs = false;
    int max_line_width = 120;
};

// Flux source code formatter.
// Walks the AST and emits canonically formatted Flux source code.
// Uses a width-aware approach: tries to emit expressions on one line,
// and expands to multi-line when the result exceeds max_line_width.
class Formatter {
public:
    using Options = FormatOptions;

    explicit Formatter(Options opts = {});

    // Format a parsed File AST into canonical source code.
    [[nodiscard]] std::string format(const File& file);

private:
    // Expression formatting
    void format_expr(const Expression& expr);
    void format_identifier(const Identifier& id);
    void format_array_expr(const ArrayExpr& expr);
    void format_dict_expr(const DictExpr& expr);
    void format_function_expr(const FunctionExpr& expr);
    void format_logical_expr(const LogicalExpr& expr);
    void format_object_expr(const ObjectExpr& expr);
    void format_member_expr(const MemberExpr& expr);
    void format_index_expr(const IndexExpr& expr);
    void format_binary_expr(const BinaryExpr& expr);
    void format_unary_expr(const UnaryExpr& expr);
    void format_pipe_expr(const PipeExpr& expr);
    void format_call_expr(const CallExpr& expr);
    void format_conditional_expr(const ConditionalExpr& expr);
    void format_string_expr(const StringExpr& expr);
    void format_paren_expr(const ParenExpr& expr);

    // Statement formatting
    void format_stmt(const Statement& stmt);
    void format_expr_stmt(const ExprStmt& stmt);
    void format_variable_assgn(const VariableAssgn& stmt);
    void format_option_stmt(const OptionStmt& stmt);
    void format_return_stmt(const ReturnStmt& stmt);
    void format_testcase_stmt(const TestCaseStmt& stmt);
    void format_builtin_stmt(const BuiltinStmt& stmt);

    // Other nodes
    void format_property(const Property& prop);
    void format_property_key(const PropertyKey& key);
    void format_block(const Block& block);
    void format_function_body(const FunctionBody& body);
    void format_assignment(const Assignment& assgn);

    // Multi-line variants (expanded formatting)
    void format_call_expr_multiline(const CallExpr& expr);
    void format_object_expr_multiline(const ObjectExpr& expr);
    void format_array_expr_multiline(const ArrayExpr& expr);

    // Try to format an expression into a single-line string (for width checking).
    // 当 inline_budget_ > 0 时启用 early-exit：超出预算即停止输出。
    [[nodiscard]] std::string try_format_inline(const Expression& expr);
    [[nodiscard]] std::string try_format_property_inline(const Property& prop);
    [[nodiscard]] std::string try_format_call_inline(const CallExpr& expr);
    [[nodiscard]] std::string try_format_object_inline(const ObjectExpr& expr);

    // Check if a call expression contains any "complex" arguments
    // (pipe expressions, nested calls with many args, objects with multiple props).
    [[nodiscard]] bool is_complex_call(const CallExpr& expr) const;
    [[nodiscard]] bool is_complex_expr(const Expression& expr) const;

    // Check if a statement is a "simple" variable assignment (single-line, non-complex init).
    [[nodiscard]] bool is_simple_variable_assgn(const Statement& stmt);

    // Helpers
    void emit(std::string_view s);
    void emit_newline();
    void emit_indent();
    void indent();
    void dedent();
    [[nodiscard]] int current_column() const;

    Options opts_;
    std::string output_;
    int indent_level_ = 0;
    // inline 宽度预算: >0 时表示正在进行 inline 测量，超出则通过 budget_exceeded_ 提前终止
    int inline_budget_ = 0;
    bool budget_exceeded_ = false;
};

} // namespace pl::flux::lsp
