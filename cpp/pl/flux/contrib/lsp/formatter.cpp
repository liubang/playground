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

#include "cpp/pl/flux/contrib/lsp/formatter.h"

#include <algorithm>
#include <ranges>
#include <vector>

namespace pl::flux::lsp {

Formatter::Formatter(Options opts) : opts_(opts) {}

std::string Formatter::format(const File& file) {
    output_.clear();
    indent_level_ = 0;

    // Package clause
    if (file.package && file.package->name) {
        if (!file.package->attributes.empty()) {
            for (const auto& attr : file.package->attributes) {
                emit(attr->string());
                emit(" ");
            }
        }
        emit("package ");
        emit(file.package->name->string());
        emit_newline();
        emit_newline();
    }

    // Imports
    if (!file.imports.empty()) {
        for (const auto& imp : file.imports) {
            if (!imp->attributes.empty()) {
                for (const auto& attr : imp->attributes) {
                    emit(attr->string());
                    emit(" ");
                }
            }
            emit("import ");
            if (imp->alias) {
                emit(imp->alias->string());
                emit(" ");
            }
            emit("\"");
            emit(imp->path->string());
            emit("\"");
            emit_newline();
        }
        emit_newline();
    }

    // Body statements
    for (size_t i = 0; i < file.body.size(); ++i) {
        format_stmt(*file.body[i]);
        emit_newline();
        // Add blank line between top-level statements, but not between
        // consecutive *simple* variable assignments (they form a logical group).
        // A "simple" assignment has a non-complex init expression (stays on one line).
        if (i + 1 < file.body.size()) {
            bool current_is_simple_var = is_simple_variable_assgn(*file.body[i]);
            bool next_is_simple_var = is_simple_variable_assgn(*file.body[i + 1]);
            if (!(current_is_simple_var && next_is_simple_var)) {
                emit_newline();
            }
        }
    }

    return output_;
}

// ============================================================
// Expression formatting
// ============================================================

void Formatter::format_expr(const Expression& expr) {
    switch (expr.type) {
        case Expression::Type::Identifier:
            format_identifier(*std::get<std::unique_ptr<Identifier>>(expr.expr));
            break;
        case Expression::Type::ArrayExpr:
            format_array_expr(*std::get<std::unique_ptr<ArrayExpr>>(expr.expr));
            break;
        case Expression::Type::DictExpr:
            format_dict_expr(*std::get<std::unique_ptr<DictExpr>>(expr.expr));
            break;
        case Expression::Type::FunctionExpr:
            format_function_expr(*std::get<std::unique_ptr<FunctionExpr>>(expr.expr));
            break;
        case Expression::Type::LogicalExpr:
            format_logical_expr(*std::get<std::unique_ptr<LogicalExpr>>(expr.expr));
            break;
        case Expression::Type::ObjectExpr:
            format_object_expr(*std::get<std::unique_ptr<ObjectExpr>>(expr.expr));
            break;
        case Expression::Type::MemberExpr:
            format_member_expr(*std::get<std::unique_ptr<MemberExpr>>(expr.expr));
            break;
        case Expression::Type::IndexExpr:
            format_index_expr(*std::get<std::unique_ptr<IndexExpr>>(expr.expr));
            break;
        case Expression::Type::BinaryExpr:
            format_binary_expr(*std::get<std::unique_ptr<BinaryExpr>>(expr.expr));
            break;
        case Expression::Type::UnaryExpr:
            format_unary_expr(*std::get<std::unique_ptr<UnaryExpr>>(expr.expr));
            break;
        case Expression::Type::PipeExpr:
            format_pipe_expr(*std::get<std::unique_ptr<PipeExpr>>(expr.expr));
            break;
        case Expression::Type::CallExpr:
            format_call_expr(*std::get<std::unique_ptr<CallExpr>>(expr.expr));
            break;
        case Expression::Type::ConditionalExpr:
            format_conditional_expr(*std::get<std::unique_ptr<ConditionalExpr>>(expr.expr));
            break;
        case Expression::Type::StringExpr:
            format_string_expr(*std::get<std::unique_ptr<StringExpr>>(expr.expr));
            break;
        case Expression::Type::ParenExpr:
            format_paren_expr(*std::get<std::unique_ptr<ParenExpr>>(expr.expr));
            break;
        case Expression::Type::IntegerLit:
            emit(std::get<std::unique_ptr<IntegerLit>>(expr.expr)->string());
            break;
        case Expression::Type::FloatLit:
            emit(std::get<std::unique_ptr<FloatLit>>(expr.expr)->string());
            break;
        case Expression::Type::StringLit: {
            const auto& lit = *std::get<std::unique_ptr<StringLit>>(expr.expr);
            emit("\"");
            emit(lit.string());
            emit("\"");
            break;
        }
        case Expression::Type::DurationLit:
            emit(std::get<std::unique_ptr<DurationLit>>(expr.expr)->string());
            break;
        case Expression::Type::UnsignedIntegerLit:
            emit(std::get<std::unique_ptr<UintLit>>(expr.expr)->string());
            break;
        case Expression::Type::BooleanLit:
            emit(std::get<std::unique_ptr<BooleanLit>>(expr.expr)->string());
            break;
        case Expression::Type::DateTimeLit:
            emit(std::get<std::unique_ptr<DateTimeLit>>(expr.expr)->string());
            break;
        case Expression::Type::RegexpLit:
            emit(std::get<std::unique_ptr<RegexpLit>>(expr.expr)->string());
            break;
        case Expression::Type::PipeLit:
            emit("<-");
            break;
        case Expression::Type::LabelLit:
            emit(std::get<std::unique_ptr<LabelLit>>(expr.expr)->string());
            break;
        case Expression::Type::BadExpr:
            emit(std::get<std::unique_ptr<BadExpr>>(expr.expr)->text);
            break;
    }
}

void Formatter::format_identifier(const Identifier& id) { emit(id.name); }

void Formatter::format_array_expr(const ArrayExpr& expr) {
    if (expr.elements.empty()) {
        emit("[]");
        return;
    }

    // Try inline first
    std::string inline_str = "[";
    for (size_t i = 0; i < expr.elements.size(); ++i) {
        if (i > 0) {
            inline_str += ", ";
        }
        inline_str += try_format_inline(*expr.elements[i]->expression);
    }
    inline_str += "]";

    if (current_column() + static_cast<int>(inline_str.size()) <= opts_.max_line_width) {
        emit(inline_str);
    } else {
        format_array_expr_multiline(expr);
    }
}

void Formatter::format_array_expr_multiline(const ArrayExpr& expr) {
    emit("[");
    indent();
    for (const auto& elem : expr.elements) {
        emit_newline();
        emit_indent();
        format_expr(*elem->expression);
        emit(",");
    }
    dedent();
    emit_newline();
    emit_indent();
    emit("]");
}

void Formatter::format_dict_expr(const DictExpr& expr) {
    if (expr.elements.empty()) {
        emit("[:]");
        return;
    }
    emit("[");
    for (size_t i = 0; i < expr.elements.size(); ++i) {
        if (i > 0) {
            emit(", ");
        }
        format_expr(*expr.elements[i]->key);
        emit(": ");
        format_expr(*expr.elements[i]->val);
    }
    emit("]");
}

void Formatter::format_function_expr(const FunctionExpr& expr) {
    emit("(");
    for (size_t i = 0; i < expr.params.size(); ++i) {
        if (i > 0) {
            emit(", ");
        }
        format_property(*expr.params[i]);
    }
    emit(") =>");
    if (expr.body) {
        emit(" ");
        format_function_body(*expr.body);
    }
}

void Formatter::format_logical_expr(const LogicalExpr& expr) {
    format_expr(*expr.left);
    emit(op_string(expr.op));
    format_expr(*expr.right);
}

void Formatter::format_object_expr(const ObjectExpr& expr) {
    if (expr.properties.empty() && !expr.with) {
        emit("{}");
        return;
    }

    // Check if any property value is complex (pipe expr, nested call, etc.)
    bool has_complex = false;
    for (const auto& prop : expr.properties) {
        if (prop->value && is_complex_expr(*prop->value)) {
            has_complex = true;
            break;
        }
    }

    if (has_complex || expr.properties.size() > 3) {
        format_object_expr_multiline(expr);
        return;
    }

    // Try inline
    std::string inline_str = try_format_object_inline(expr);
    if (current_column() + static_cast<int>(inline_str.size()) <= opts_.max_line_width) {
        emit(inline_str);
    } else {
        format_object_expr_multiline(expr);
    }
}

void Formatter::format_object_expr_multiline(const ObjectExpr& expr) {
    emit("{");
    indent();
    if (expr.with) {
        emit_newline();
        emit_indent();
        emit(expr.with->source->string());
        emit(" with");
    }
    for (const auto& prop : expr.properties) {
        emit_newline();
        emit_indent();
        format_property(*prop);
        emit(",");
    }
    dedent();
    emit_newline();
    emit_indent();
    emit("}");
}

void Formatter::format_member_expr(const MemberExpr& expr) {
    format_expr(*expr.object);
    emit(".");
    format_property_key(*expr.property);
}

void Formatter::format_index_expr(const IndexExpr& expr) {
    format_expr(*expr.array);
    emit("[");
    format_expr(*expr.index);
    emit("]");
}

void Formatter::format_binary_expr(const BinaryExpr& expr) {
    format_expr(*expr.left);
    emit(" ");
    emit(op_string(expr.op));
    emit(" ");
    format_expr(*expr.right);
}

void Formatter::format_unary_expr(const UnaryExpr& expr) {
    emit(op_string(expr.op));
    format_expr(*expr.argument);
}

void Formatter::format_pipe_expr(const PipeExpr& expr) {
    // Flatten the pipe chain: a |> b() |> c() is PipeExpr(PipeExpr(a, b), c)
    // Collect all pipe steps into a flat list.
    std::vector<const CallExpr*> steps;
    const PipeExpr* current = &expr;

    // Walk down the left-recursive pipe chain
    while (true) {
        steps.push_back(current->call.get());
        if (current->argument->type == Expression::Type::PipeExpr) {
            current = std::get<std::unique_ptr<PipeExpr>>(current->argument->expr).get();
        } else {
            break;
        }
    }

    // steps is in reverse order (outermost first collected, innermost last), reverse it
    std::ranges::reverse(steps);

    // Format the base expression (the leftmost non-pipe expr)
    format_expr(*current->argument);

    // Format each pipe step at the same indentation level (one level deeper than base)
    indent();
    for (const auto* call : steps) {
        emit_newline();
        emit_indent();
        emit("|> ");
        format_call_expr(*call);
    }
    dedent();
}

void Formatter::format_call_expr(const CallExpr& expr) {
    // Expand to multi-line if the call contains structurally complex arguments
    // (pipe exprs, functions, deeply nested objects).
    if (is_complex_call(expr)) {
        format_call_expr_multiline(expr);
        return;
    }

    // Try inline first, then check if it exceeds max_line_width.
    // If so, expand to multi-line.
    std::string inline_str = try_format_call_inline(expr);
    if (current_column() + static_cast<int>(inline_str.size()) > opts_.max_line_width) {
        format_call_expr_multiline(expr);
        return;
    }

    // Inline: unwrap single ObjectExpr argument
    format_expr(*expr.callee);
    emit("(");
    if (expr.arguments.size() == 1 && expr.arguments[0]->type == Expression::Type::ObjectExpr) {
        const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(expr.arguments[0]->expr);
        for (size_t i = 0; i < obj.properties.size(); ++i) {
            if (i > 0) {
                emit(", ");
            }
            format_property(*obj.properties[i]);
        }
    } else {
        for (size_t i = 0; i < expr.arguments.size(); ++i) {
            if (i > 0) {
                emit(", ");
            }
            format_expr(*expr.arguments[i]);
        }
    }
    emit(")");
}

void Formatter::format_call_expr_multiline(const CallExpr& expr) {
    format_expr(*expr.callee);
    emit("(");

    if (expr.arguments.empty()) {
        emit(")");
        return;
    }

    // For call arguments, which are typically ObjectExpr nodes wrapping named params,
    // we check if there's a single ObjectExpr argument (the common pattern in Flux).
    if (expr.arguments.size() == 1 && expr.arguments[0]->type == Expression::Type::ObjectExpr) {
        const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(expr.arguments[0]->expr);
        // Expand the object's properties directly as call arguments
        indent();
        for (const auto& prop : obj.properties) {
            emit_newline();
            emit_indent();
            format_property(*prop);
            emit(",");
        }
        dedent();
        emit_newline();
        emit_indent();
        emit(")");
    } else {
        indent();
        for (const auto& arg : expr.arguments) {
            emit_newline();
            emit_indent();
            format_expr(*arg);
            emit(",");
        }
        dedent();
        emit_newline();
        emit_indent();
        emit(")");
    }
}

void Formatter::format_conditional_expr(const ConditionalExpr& expr) {
    emit("if ");
    format_expr(*expr.test);
    emit(" then ");
    format_expr(*expr.consequent);
    emit(" else ");
    format_expr(*expr.alternate);
}

void Formatter::format_string_expr(const StringExpr& expr) {
    emit("\"");
    for (const auto& part : expr.parts) {
        switch (part->type) {
            case StringExprPart::Type::Text:
                emit(std::get<std::unique_ptr<TextPart>>(part->part)->value);
                break;
            case StringExprPart::Type::Interpolated:
                emit("${");
                format_expr(*std::get<std::unique_ptr<InterpolatedPart>>(part->part)->expression);
                emit("}");
                break;
        }
    }
    emit("\"");
}

void Formatter::format_paren_expr(const ParenExpr& expr) {
    emit("(");
    format_expr(*expr.expression);
    emit(")");
}

// ============================================================
// Statement formatting
// ============================================================

void Formatter::format_stmt(const Statement& stmt) {
    if (!stmt.attributes.empty()) {
        for (const auto& attr : stmt.attributes) {
            emit(attr->string());
            emit(" ");
        }
    }

    switch (stmt.type) {
        case Statement::Type::ExpressionStatement:
            format_expr_stmt(*std::get<std::unique_ptr<ExprStmt>>(stmt.stmt));
            break;
        case Statement::Type::VariableAssignment:
            format_variable_assgn(*std::get<std::unique_ptr<VariableAssgn>>(stmt.stmt));
            break;
        case Statement::Type::OptionStatement:
            format_option_stmt(*std::get<std::unique_ptr<OptionStmt>>(stmt.stmt));
            break;
        case Statement::Type::ReturnStatement:
            format_return_stmt(*std::get<std::unique_ptr<ReturnStmt>>(stmt.stmt));
            break;
        case Statement::Type::BadStatement:
            emit(std::get<std::unique_ptr<BadStmt>>(stmt.stmt)->text);
            break;
        case Statement::Type::TestCaseStatement:
            format_testcase_stmt(*std::get<std::unique_ptr<TestCaseStmt>>(stmt.stmt));
            break;
        case Statement::Type::BuiltinStatement:
            format_builtin_stmt(*std::get<std::unique_ptr<BuiltinStmt>>(stmt.stmt));
            break;
    }
}

void Formatter::format_expr_stmt(const ExprStmt& stmt) { format_expr(*stmt.expression); }

void Formatter::format_variable_assgn(const VariableAssgn& stmt) {
    emit(stmt.id->string());
    emit(" = ");
    format_expr(*stmt.init);
}

void Formatter::format_option_stmt(const OptionStmt& stmt) {
    emit("option ");
    format_assignment(*stmt.assignment);
}

void Formatter::format_return_stmt(const ReturnStmt& stmt) {
    emit("return ");
    format_expr(*stmt.argument);
}

void Formatter::format_testcase_stmt(const TestCaseStmt& stmt) {
    emit("testcase ");
    emit(stmt.id->string());
    if (stmt.extends) {
        emit(" extends \"");
        emit(stmt.extends->string());
        emit("\"");
    }
    emit(" ");
    format_block(*stmt.block);
}

void Formatter::format_builtin_stmt(const BuiltinStmt& stmt) {
    // BuiltinStmt::string() already produces "builtin <id>: <type>" format
    emit(stmt.string());
}

// ============================================================
// Other node formatting
// ============================================================

void Formatter::format_property(const Property& prop) {
    format_property_key(*prop.key);
    if (prop.value) {
        emit(": ");
        format_expr(*prop.value);
    }
}

void Formatter::format_property_key(const PropertyKey& key) {
    switch (key.type) {
        case PropertyKey::Type::Identifier:
            emit(std::get<std::unique_ptr<Identifier>>(key.key)->string());
            break;
        case PropertyKey::Type::StringLiteral: {
            const auto& lit = *std::get<std::unique_ptr<StringLit>>(key.key);
            emit("\"");
            emit(lit.string());
            emit("\"");
            break;
        }
    }
}

void Formatter::format_block(const Block& block) {
    if (block.body.empty()) {
        emit("{}");
        return;
    }
    emit("{");
    indent();
    for (const auto& stmt : block.body) {
        emit_newline();
        emit_indent();
        format_stmt(*stmt);
    }
    dedent();
    emit_newline();
    emit_indent();
    emit("}");
}

void Formatter::format_function_body(const FunctionBody& body) {
    switch (body.type) {
        case FunctionBody::Type::Block:
            format_block(*std::get<std::unique_ptr<Block>>(body.body));
            break;
        case FunctionBody::Type::Expression:
            format_expr(*std::get<std::unique_ptr<Expression>>(body.body));
            break;
    }
}

void Formatter::format_assignment(const Assignment& assgn) {
    switch (assgn.type) {
        case Assignment::Type::VariableAssignment:
            format_variable_assgn(*std::get<std::unique_ptr<VariableAssgn>>(assgn.value));
            break;
        case Assignment::Type::MemberAssignment: {
            const auto& ma = *std::get<std::unique_ptr<MemberAssgn>>(assgn.value);
            format_member_expr(*ma.member);
            emit(" = ");
            format_expr(*ma.init);
            break;
        }
    }
}

// ============================================================
// Inline (single-line) formatting for width estimation
// ============================================================

std::string Formatter::try_format_inline(const Expression& expr) {
    Formatter tmp(opts_);
    tmp.indent_level_ = 0;
    // 设置预算: 当前行剩余可用宽度
    tmp.inline_budget_ = opts_.max_line_width;
    tmp.format_expr(expr);
    if (tmp.budget_exceeded_) {
        return tmp.output_; // 超限，调用方会检测到长度超宽并 fallback
    }
    for (auto c : tmp.output_) {
        if (c == '\n') {
            return tmp.output_;
        }
    }
    return tmp.output_;
}

std::string Formatter::try_format_property_inline(const Property& prop) {
    Formatter tmp(opts_);
    tmp.indent_level_ = 0;
    tmp.inline_budget_ = opts_.max_line_width;
    tmp.format_property(prop);
    return tmp.output_;
}

std::string Formatter::try_format_call_inline(const CallExpr& expr) {
    Formatter tmp(opts_);
    tmp.indent_level_ = 0;
    tmp.inline_budget_ = opts_.max_line_width;
    tmp.format_expr(*expr.callee);
    tmp.emit("(");

    if (tmp.budget_exceeded_) {
        return tmp.output_;
    }

    // Unwrap single ObjectExpr argument (common Flux pattern: named params)
    if (expr.arguments.size() == 1 && expr.arguments[0]->type == Expression::Type::ObjectExpr) {
        const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(expr.arguments[0]->expr);
        for (size_t i = 0; i < obj.properties.size(); ++i) {
            if (tmp.budget_exceeded_) {
                return tmp.output_;
            }
            if (i > 0) {
                tmp.emit(", ");
            }
            tmp.format_property(*obj.properties[i]);
        }
    } else {
        for (size_t i = 0; i < expr.arguments.size(); ++i) {
            if (tmp.budget_exceeded_) {
                return tmp.output_;
            }
            if (i > 0) {
                tmp.emit(", ");
            }
            tmp.format_expr(*expr.arguments[i]);
        }
    }

    tmp.emit(")");
    for (auto c : tmp.output_) {
        if (c == '\n') {
            return tmp.output_;
        }
    }
    return tmp.output_;
}

std::string Formatter::try_format_object_inline(const ObjectExpr& expr) {
    Formatter tmp(opts_);
    tmp.indent_level_ = 0;
    tmp.inline_budget_ = opts_.max_line_width;
    tmp.emit("{");
    if (expr.with) {
        tmp.emit(expr.with->source->string());
        tmp.emit(" with ");
    }
    for (size_t i = 0; i < expr.properties.size(); ++i) {
        if (tmp.budget_exceeded_) {
            return tmp.output_;
        }
        if (i > 0) {
            tmp.emit(", ");
        }
        tmp.format_property(*expr.properties[i]);
    }
    tmp.emit("}");
    return tmp.output_;
}

// ============================================================
// Complexity checks
// ============================================================

bool Formatter::is_complex_call(const CallExpr& expr) const {
    // Unwrap single ObjectExpr (Flux named params pattern)
    if (expr.arguments.size() == 1 && expr.arguments[0]->type == Expression::Type::ObjectExpr) {
        const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(expr.arguments[0]->expr);
        return std::ranges::any_of(obj.properties, [this](const auto& prop) {
            return prop->value && is_complex_expr(*prop->value);
        });
    }
    // A call is complex if any argument is complex
    return std::ranges::any_of(expr.arguments, [this](const auto& arg) {
        return is_complex_expr(*arg);
    });
}

bool Formatter::is_complex_expr(const Expression& expr) const {
    switch (expr.type) {
        case Expression::Type::PipeExpr:
            return true;
        case Expression::Type::CallExpr: {
            const auto& call = *std::get<std::unique_ptr<CallExpr>>(expr.expr);
            // A nested call with arguments is complex if its args are complex
            if (!call.arguments.empty()) {
                for (const auto& arg : call.arguments) {
                    if (is_complex_expr(*arg)) {
                        return true;
                    }
                }
            }
            return false;
        }
        case Expression::Type::ObjectExpr: {
            const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(expr.expr);
            // An object with properties that themselves contain pipe/call exprs is complex
            if (obj.properties.size() > 3) {
                return true;
            }
            return std::ranges::any_of(obj.properties, [this](const auto& prop) {
                return prop->value && is_complex_expr(*prop->value);
            });
        }
        case Expression::Type::FunctionExpr: {
            const auto& fn = *std::get<std::unique_ptr<FunctionExpr>>(expr.expr);
            // A function is only complex if it has a block body (multi-statement).
            // Simple arrow functions like `(r) => r.x == "y"` are not complex.
            return fn.body && fn.body->type == FunctionBody::Type::Block;
        }
        default:
            return false;
    }
}

// ============================================================
// Statement classification
// ============================================================

bool Formatter::is_simple_variable_assgn(const Statement& stmt) {
    if (stmt.type != Statement::Type::VariableAssignment) {
        return false;
    }
    const auto& va = *std::get<std::unique_ptr<VariableAssgn>>(stmt.stmt);
    if (!va.init) {
        return true;
    }
    // If the init expression is complex (would expand to multi-line), it's not simple.
    if (is_complex_expr(*va.init)) {
        return false;
    }
    // Also check if it's a call expression that would exceed line width
    if (va.init->type == Expression::Type::CallExpr) {
        const auto& call = *std::get<std::unique_ptr<CallExpr>>(va.init->expr);
        std::string inline_str = try_format_call_inline(call);
        // Estimate: "name = " prefix + call inline
        int estimated_width =
            static_cast<int>(va.id->name.size()) + 3 + static_cast<int>(inline_str.size());
        if (estimated_width > opts_.max_line_width) {
            return false;
        }
    }
    return true;
}

// ============================================================
// Helpers
// ============================================================

void Formatter::emit(std::string_view s) {
    output_.append(s);
    // inline 宽度预算检查: 超出预算时标记终止
    if (inline_budget_ > 0 && output_.size() > static_cast<size_t>(inline_budget_)) {
        budget_exceeded_ = true;
    }
}

void Formatter::emit_newline() { output_.push_back('\n'); }

void Formatter::emit_indent() {
    for (int i = 0; i < indent_level_; ++i) {
        if (opts_.use_tabs) {
            output_.push_back('\t');
        } else {
            output_.append(static_cast<size_t>(opts_.indent_width), ' ');
        }
    }
}

void Formatter::indent() { ++indent_level_; }

void Formatter::dedent() { --indent_level_; }

int Formatter::current_column() const {
    // Find the last newline and calculate column from there
    auto pos = output_.rfind('\n');
    if (pos == std::string::npos) {
        return static_cast<int>(output_.size());
    }
    return static_cast<int>(output_.size() - pos - 1);
}

} // namespace pl::flux::lsp
