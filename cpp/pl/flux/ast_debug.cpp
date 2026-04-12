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

#include "ast_debug.h"

#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_join.h"

namespace pl {
namespace {

std::string attributes_summary(const std::vector<std::shared_ptr<Attribute>>& attributes) {
    if (attributes.empty()) {
        return "";
    }
    return " attrs=" +
           absl::StrJoin(attributes, ", ", [](std::string* out, const auto& attr) {
               out->append(attr->string());
           });
}

std::string location_summary(const SourceLocation& loc) {
    if (!loc.is_valid()) {
        return "";
    }
    std::ostringstream ss;
    ss << " loc=" << loc.start.line << ":" << loc.start.column << "-" << loc.end.line << ":"
       << loc.end.column;
    return ss.str();
}

class AstDumper {
public:
    std::string dump(const File& file) {
        line("", true, "File name=\"" + file.name + "\"" + location_summary(file.loc));
        std::vector<const ImportDeclaration*> imports;
        imports.reserve(file.imports.size());
        for (const auto& import : file.imports) {
            imports.push_back(import.get());
        }

        const bool has_package = file.package != nullptr;
        const size_t top_count = (has_package ? 1 : 0) + imports.size() + file.body.size();
        size_t index = 0;

        if (file.package) {
            dump_package_clause(*file.package, "", ++index == top_count);
        }
        for (const auto* import : imports) {
            dump_import(*import, "", ++index == top_count);
        }
        for (const auto& stmt : file.body) {
            dump_statement(*stmt, "", ++index == top_count);
        }
        return out_.str();
    }

private:
    static std::string child_prefix(const std::string& prefix, bool is_last) {
        return prefix + (is_last ? "   " : "|  ");
    }

    void line(const std::string& prefix, bool is_last, const std::string& text) {
        out_ << prefix << (prefix.empty() ? "" : (is_last ? "`- " : "|- ")) << text << '\n';
    }

    void dump_package_clause(const PackageClause& package,
                             const std::string& prefix,
                             bool is_last) {
        line(prefix, is_last,
             "PackageClause name=" + package.name->string() + attributes_summary(package.attributes) +
                 location_summary(package.loc));
    }

    void dump_import(const ImportDeclaration& import, const std::string& prefix, bool is_last) {
        std::string label = "ImportDeclaration";
        if (import.alias) {
            label += " alias=" + import.alias->string();
        }
        if (import.path) {
            label += " path=\"" + import.path->value + "\"";
        }
        label += attributes_summary(import.attributes);
        label += location_summary(import.loc);
        line(prefix, is_last, label);
    }

    void dump_statement(const Statement& stmt, const std::string& prefix, bool is_last) {
        const std::string attrs = attributes_summary(stmt.attributes);
        const std::string loc = location_summary(stmt.loc);
        switch (stmt.type) {
            case Statement::Type::ExpressionStatement: {
                const auto& expr = std::get<std::unique_ptr<ExprStmt>>(stmt.stmt);
                line(prefix, is_last, "ExpressionStatement" + attrs + loc);
                dump_expression(*expr->expression, child_prefix(prefix, is_last), true);
                break;
            }
            case Statement::Type::VariableAssignment: {
                const auto& assign = std::get<std::unique_ptr<VariableAssgn>>(stmt.stmt);
                line(prefix, is_last, "VariableAssignment id=" + assign->id->string() + attrs + loc);
                dump_expression(*assign->init, child_prefix(prefix, is_last), true);
                break;
            }
            case Statement::Type::OptionStatement: {
                const auto& option = std::get<std::unique_ptr<OptionStmt>>(stmt.stmt);
                line(prefix, is_last, "OptionStatement" + attrs + loc);
                dump_assignment(*option->assignment, child_prefix(prefix, is_last), true);
                break;
            }
            case Statement::Type::ReturnStatement: {
                const auto& ret = std::get<std::unique_ptr<ReturnStmt>>(stmt.stmt);
                line(prefix, is_last, "ReturnStatement" + attrs + loc);
                dump_expression(*ret->argument, child_prefix(prefix, is_last), true);
                break;
            }
            case Statement::Type::BadStatement: {
                const auto& bad = std::get<std::unique_ptr<BadStmt>>(stmt.stmt);
                line(prefix, is_last, "BadStatement text=\"" + bad->text + "\"" + attrs + loc);
                break;
            }
            case Statement::Type::TestCaseStatement: {
                const auto& testcase = std::get<std::unique_ptr<TestCaseStmt>>(stmt.stmt);
                std::string label = "TestCaseStatement id=" + testcase->id->string() + attrs + loc;
                if (testcase->extends) {
                    label += " extends=\"" + testcase->extends->value + "\"";
                }
                line(prefix, is_last, label);
                dump_block(*testcase->block, child_prefix(prefix, is_last), true);
                break;
            }
            case Statement::Type::BuiltinStatement: {
                const auto& builtin = std::get<std::unique_ptr<BuiltinStmt>>(stmt.stmt);
                line(prefix, is_last, "BuiltinStatement id=" + builtin->id->string() + attrs + loc);
                dump_type_expression(*builtin->ty, child_prefix(prefix, is_last), true);
                break;
            }
        }
    }

    void dump_assignment(const Assignment& assignment, const std::string& prefix, bool is_last) {
        switch (assignment.type) {
            case Assignment::Type::VariableAssignment: {
                const auto& assign = std::get<std::unique_ptr<VariableAssgn>>(assignment.value);
                line(prefix, is_last, "VariableAssignment id=" + assign->id->string());
                dump_expression(*assign->init, child_prefix(prefix, is_last), true);
                break;
            }
            case Assignment::Type::MemberAssignment: {
                const auto& assign = std::get<std::unique_ptr<MemberAssgn>>(assignment.value);
                line(prefix, is_last, "MemberAssignment");
                auto next = child_prefix(prefix, is_last);
                dump_member_expr(*assign->member, next, false);
                dump_expression(*assign->init, next, true);
                break;
            }
        }
    }

    void dump_block(const Block& block, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "Block" + location_summary(block.loc));
        auto next = child_prefix(prefix, is_last);
        for (size_t i = 0; i < block.body.size(); ++i) {
            dump_statement(*block.body[i], next, i + 1 == block.body.size());
        }
    }

    void dump_property(const Property& property, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "Property key=" + property.key->string());
        if (property.value) {
            dump_expression(*property.value, child_prefix(prefix, is_last), true);
        }
    }

    void dump_expression(const Expression& expr, const std::string& prefix, bool is_last) {
        switch (expr.type) {
            case Expression::Type::Identifier:
                line(prefix, is_last,
                     "Identifier name=" +
                         std::get<std::unique_ptr<Identifier>>(expr.expr)->string());
                break;
            case Expression::Type::ArrayExpr:
                dump_array_expr(*std::get<std::unique_ptr<ArrayExpr>>(expr.expr), prefix, is_last);
                break;
            case Expression::Type::DictExpr:
                dump_dict_expr(*std::get<std::unique_ptr<DictExpr>>(expr.expr), prefix, is_last);
                break;
            case Expression::Type::FunctionExpr:
                dump_function_expr(*std::get<std::unique_ptr<FunctionExpr>>(expr.expr), prefix,
                                   is_last);
                break;
            case Expression::Type::LogicalExpr:
                dump_logical_expr(*std::get<std::unique_ptr<LogicalExpr>>(expr.expr), prefix,
                                  is_last);
                break;
            case Expression::Type::ObjectExpr:
                dump_object_expr(*std::get<std::unique_ptr<ObjectExpr>>(expr.expr), prefix,
                                 is_last);
                break;
            case Expression::Type::MemberExpr:
                dump_member_expr(*std::get<std::unique_ptr<MemberExpr>>(expr.expr), prefix,
                                 is_last);
                break;
            case Expression::Type::IndexExpr:
                dump_index_expr(*std::get<std::unique_ptr<IndexExpr>>(expr.expr), prefix, is_last);
                break;
            case Expression::Type::BinaryExpr:
                dump_binary_expr(*std::get<std::unique_ptr<BinaryExpr>>(expr.expr), prefix,
                                 is_last);
                break;
            case Expression::Type::UnaryExpr:
                dump_unary_expr(*std::get<std::unique_ptr<UnaryExpr>>(expr.expr), prefix, is_last);
                break;
            case Expression::Type::PipeExpr:
                dump_pipe_expr(*std::get<std::unique_ptr<PipeExpr>>(expr.expr), prefix, is_last);
                break;
            case Expression::Type::CallExpr:
                dump_call_expr(*std::get<std::unique_ptr<CallExpr>>(expr.expr), prefix, is_last);
                break;
            case Expression::Type::ConditionalExpr:
                dump_conditional_expr(*std::get<std::unique_ptr<ConditionalExpr>>(expr.expr),
                                      prefix, is_last);
                break;
            case Expression::Type::StringExpr:
                dump_string_expr(*std::get<std::unique_ptr<StringExpr>>(expr.expr), prefix,
                                 is_last);
                break;
            case Expression::Type::ParenExpr: {
                const auto& paren = *std::get<std::unique_ptr<ParenExpr>>(expr.expr);
                line(prefix, is_last, "ParenExpr");
                dump_expression(*paren.expression, child_prefix(prefix, is_last), true);
                break;
            }
            case Expression::Type::IntegerLit:
                line(prefix, is_last,
                     "IntegerLit value=" +
                         std::get<std::unique_ptr<IntegerLit>>(expr.expr)->string());
                break;
            case Expression::Type::FloatLit:
                line(prefix, is_last,
                     "FloatLit value=" + std::get<std::unique_ptr<FloatLit>>(expr.expr)->string());
                break;
            case Expression::Type::StringLit:
                line(prefix, is_last,
                     "StringLit value=\"" + std::get<std::unique_ptr<StringLit>>(expr.expr)->value +
                         "\"");
                break;
            case Expression::Type::DurationLit:
                line(prefix, is_last,
                     "DurationLit value=" +
                         std::get<std::unique_ptr<DurationLit>>(expr.expr)->string());
                break;
            case Expression::Type::UnsignedIntegerLit:
                line(prefix, is_last,
                     "UintLit value=" + std::get<std::unique_ptr<UintLit>>(expr.expr)->string());
                break;
            case Expression::Type::BooleanLit:
                line(prefix, is_last,
                     "BooleanLit value=" +
                         std::get<std::unique_ptr<BooleanLit>>(expr.expr)->string());
                break;
            case Expression::Type::DateTimeLit:
                line(prefix, is_last,
                     "DateTimeLit value=" +
                         std::get<std::unique_ptr<DateTimeLit>>(expr.expr)->string());
                break;
            case Expression::Type::RegexpLit:
                line(prefix, is_last,
                     "RegexpLit value=" +
                         std::get<std::unique_ptr<RegexpLit>>(expr.expr)->string());
                break;
            case Expression::Type::PipeLit:
                line(prefix, is_last, "PipeLit");
                break;
            case Expression::Type::LabelLit:
                line(prefix, is_last,
                     "LabelLit value=" + std::get<std::unique_ptr<LabelLit>>(expr.expr)->string());
                break;
            case Expression::Type::BadExpr:
                line(prefix, is_last,
                     "BadExpr text=\"" + std::get<std::unique_ptr<BadExpr>>(expr.expr)->text +
                         "\"" + location_summary(expr.loc));
                break;
        }
    }

    void dump_array_expr(const ArrayExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "ArrayExpr");
        auto next = child_prefix(prefix, is_last);
        for (size_t i = 0; i < expr.elements.size(); ++i) {
            dump_expression(*expr.elements[i]->expression, next, i + 1 == expr.elements.size());
        }
    }

    void dump_dict_expr(const DictExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "DictExpr");
        auto next = child_prefix(prefix, is_last);
        for (size_t i = 0; i < expr.elements.size(); ++i) {
            const auto& item = expr.elements[i];
            line(next, i + 1 == expr.elements.size(), "DictItem");
            auto item_prefix = child_prefix(next, i + 1 == expr.elements.size());
            dump_expression(*item->key, item_prefix, false);
            dump_expression(*item->val, item_prefix, true);
        }
    }

    void dump_function_expr(const FunctionExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "FunctionExpr");
        auto next = child_prefix(prefix, is_last);
        line(next, expr.params.empty(), "Parameters");
        auto params_prefix = child_prefix(next, expr.params.empty());
        for (size_t i = 0; i < expr.params.size(); ++i) {
            dump_property(*expr.params[i], params_prefix, i + 1 == expr.params.size());
        }
        if (expr.body->type == FunctionBody::Type::Block) {
            dump_block(*std::get<std::unique_ptr<Block>>(expr.body->body), next, true);
        } else {
            dump_expression(*std::get<std::unique_ptr<Expression>>(expr.body->body), next, true);
        }
    }

    void dump_logical_expr(const LogicalExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "LogicalExpr op=" + op_string(expr.op));
        auto next = child_prefix(prefix, is_last);
        dump_expression(*expr.left, next, false);
        dump_expression(*expr.right, next, true);
    }

    void dump_object_expr(const ObjectExpr& expr, const std::string& prefix, bool is_last) {
        std::string label = "ObjectExpr";
        if (expr.with) {
            label += " with=" + expr.with->string();
        }
        line(prefix, is_last, label);
        auto next = child_prefix(prefix, is_last);
        for (size_t i = 0; i < expr.properties.size(); ++i) {
            dump_property(*expr.properties[i], next, i + 1 == expr.properties.size());
        }
    }

    void dump_member_expr(const MemberExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "MemberExpr property=" + expr.property->string());
        dump_expression(*expr.object, child_prefix(prefix, is_last), true);
    }

    void dump_index_expr(const IndexExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "IndexExpr");
        auto next = child_prefix(prefix, is_last);
        dump_expression(*expr.array, next, false);
        dump_expression(*expr.index, next, true);
    }

    void dump_binary_expr(const BinaryExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "BinaryExpr op=" + op_string(expr.op));
        auto next = child_prefix(prefix, is_last);
        dump_expression(*expr.left, next, false);
        dump_expression(*expr.right, next, true);
    }

    void dump_unary_expr(const UnaryExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "UnaryExpr op=" + op_string(expr.op));
        dump_expression(*expr.argument, child_prefix(prefix, is_last), true);
    }

    void dump_pipe_expr(const PipeExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "PipeExpr");
        auto next = child_prefix(prefix, is_last);
        dump_expression(*expr.argument, next, false);
        dump_call_expr(*expr.call, next, true);
    }

    void dump_call_expr(const CallExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "CallExpr");
        auto next = child_prefix(prefix, is_last);
        dump_expression(*expr.callee, next, expr.arguments.empty());
        for (size_t i = 0; i < expr.arguments.size(); ++i) {
            dump_expression(*expr.arguments[i], next, i + 1 == expr.arguments.size());
        }
    }

    void dump_conditional_expr(const ConditionalExpr& expr,
                               const std::string& prefix,
                               bool is_last) {
        line(prefix, is_last, "ConditionalExpr");
        auto next = child_prefix(prefix, is_last);
        line(next, false, "Test");
        dump_expression(*expr.test, child_prefix(next, false), true);
        line(next, false, "Consequent");
        dump_expression(*expr.consequent, child_prefix(next, false), true);
        line(next, true, "Alternate");
        dump_expression(*expr.alternate, child_prefix(next, true), true);
    }

    void dump_string_expr(const StringExpr& expr, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "StringExpr");
        auto next = child_prefix(prefix, is_last);
        for (size_t i = 0; i < expr.parts.size(); ++i) {
            const auto& part = expr.parts[i];
            switch (part->type) {
                case StringExprPart::Type::Text:
                    line(next, i + 1 == expr.parts.size(),
                         "TextPart value=\"" +
                             std::get<std::unique_ptr<TextPart>>(part->part)->value + "\"");
                    break;
                case StringExprPart::Type::Interpolated:
                    line(next, i + 1 == expr.parts.size(), "InterpolatedPart");
                    dump_expression(
                        *std::get<std::unique_ptr<InterpolatedPart>>(part->part)->expression,
                        child_prefix(next, i + 1 == expr.parts.size()), true);
                    break;
            }
        }
    }

    void dump_type_expression(const TypeExpression& type_expr,
                              const std::string& prefix,
                              bool is_last) {
        line(prefix, is_last, "TypeExpression");
        auto next = child_prefix(prefix, is_last);
        dump_monotype(*type_expr.monotype, next, type_expr.constraints.empty());
        for (size_t i = 0; i < type_expr.constraints.size(); ++i) {
            dump_type_constraint(*type_expr.constraints[i], next,
                                 i + 1 == type_expr.constraints.size());
        }
    }

    void dump_type_constraint(const TypeConstraint& constraint,
                              const std::string& prefix,
                              bool is_last) {
        std::string label = "TypeConstraint tvar=" + constraint.tvar->string();
        if (!constraint.kinds.empty()) {
            label += " kinds=";
            for (size_t i = 0; i < constraint.kinds.size(); ++i) {
                if (i != 0) {
                    label += "+";
                }
                label += constraint.kinds[i]->string();
            }
        }
        line(prefix, is_last, label);
    }

    void dump_monotype(const MonoType& mono, const std::string& prefix, bool is_last) {
        switch (mono.type) {
            case MonoType::Type::Tvar:
                line(prefix, is_last,
                     "MonoType Tvar name=" +
                         std::get<std::unique_ptr<TvarType>>(mono.value)->name->string());
                break;
            case MonoType::Type::Basic:
                line(prefix, is_last,
                     "MonoType Basic name=" +
                         std::get<std::unique_ptr<NamedType>>(mono.value)->name->string());
                break;
            case MonoType::Type::Array: {
                line(prefix, is_last, "MonoType Array");
                dump_monotype(*std::get<std::unique_ptr<ArrayType>>(mono.value)->element,
                              child_prefix(prefix, is_last), true);
                break;
            }
            case MonoType::Type::Stream: {
                line(prefix, is_last, "MonoType Stream");
                dump_monotype(*std::get<std::unique_ptr<StreamType>>(mono.value)->element,
                              child_prefix(prefix, is_last), true);
                break;
            }
            case MonoType::Type::Vector: {
                line(prefix, is_last, "MonoType Vector");
                dump_monotype(*std::get<std::unique_ptr<VectorType>>(mono.value)->element,
                              child_prefix(prefix, is_last), true);
                break;
            }
            case MonoType::Type::Dict: {
                line(prefix, is_last, "MonoType Dict");
                auto next = child_prefix(prefix, is_last);
                dump_monotype(*std::get<std::unique_ptr<DictType>>(mono.value)->key, next, false);
                dump_monotype(*std::get<std::unique_ptr<DictType>>(mono.value)->val, next, true);
                break;
            }
            case MonoType::Type::Dynamic:
                line(prefix, is_last, "MonoType Dynamic");
                break;
            case MonoType::Type::Record:
                dump_record_type(*std::get<std::unique_ptr<RecordType>>(mono.value), prefix,
                                 is_last);
                break;
            case MonoType::Type::Function:
                dump_function_type(*std::get<std::unique_ptr<FunctionType>>(mono.value), prefix,
                                   is_last);
                break;
            case MonoType::Type::Label:
                line(prefix, is_last,
                     "MonoType Label value=" +
                         std::get<std::unique_ptr<LabelLit>>(mono.value)->string());
                break;
        }
    }

    void dump_record_type(const RecordType& record, const std::string& prefix, bool is_last) {
        std::string label = "MonoType Record";
        if (record.tvar) {
            label += " with=" + record.tvar->string();
        }
        line(prefix, is_last, label);
        auto next = child_prefix(prefix, is_last);
        for (size_t i = 0; i < record.properties.size(); ++i) {
            line(next, i + 1 == record.properties.size(),
                 "PropertyType name=" + record.properties[i]->name->string());
            dump_monotype(*record.properties[i]->monotype,
                          child_prefix(next, i + 1 == record.properties.size()), true);
        }
    }

    void dump_function_type(const FunctionType& function, const std::string& prefix, bool is_last) {
        line(prefix, is_last, "MonoType Function");
        auto next = child_prefix(prefix, is_last);
        for (size_t i = 0; i < function.parameters.size(); ++i) {
            const auto& param = function.parameters[i];
            switch (param->type) {
                case ParameterType::Type::Required: {
                    const auto& required = std::get<std::shared_ptr<Required>>(param->value);
                    line(next, false, "RequiredParam name=" + required->name->string());
                    dump_monotype(*required->monotype, child_prefix(next, false), true);
                    break;
                }
                case ParameterType::Type::Optional: {
                    const auto& optional = std::get<std::unique_ptr<Optional>>(param->value);
                    line(next, false, "OptionalParam name=" + optional->name->string());
                    dump_monotype(*optional->monotype, child_prefix(next, false), true);
                    break;
                }
                case ParameterType::Type::Pipe: {
                    const auto& pipe = std::get<std::unique_ptr<Pipe>>(param->value);
                    line(next, false, "PipeParam name=" + pipe->name->string());
                    dump_monotype(*pipe->monotype, child_prefix(next, false), true);
                    break;
                }
            }
        }
        dump_monotype(*function.monotype, next, true);
    }

    std::ostringstream out_;
};

std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

class AstJsonDumper {
public:
    std::string dump(const File& file) {
        begin_object();
        field("type", "File", true);
        field("summary", "name=" + file.name + location_summary(file.loc), true);
        key("children");
        begin_array();
        bool first = true;
        if (file.package) {
            element_prefix(first);
            dump_package_clause(*file.package);
        }
        for (const auto& import : file.imports) {
            element_prefix(first);
            dump_import(*import);
        }
        for (const auto& stmt : file.body) {
            element_prefix(first);
            dump_statement(*stmt);
        }
        end_array();
        end_object();
        return out_.str();
    }

private:
    void begin_object() { out_ << "{"; }
    void end_object() { out_ << "}"; }
    void begin_array() { out_ << "["; }
    void end_array() { out_ << "]"; }
    void key(std::string_view value) { out_ << "\"" << json_escape(value) << "\":"; }
    void comma() { out_ << ","; }
    void field(std::string_view name, std::string_view value, bool with_comma = false) {
        key(name);
        out_ << "\"" << json_escape(value) << "\"";
        if (with_comma) {
            comma();
        }
    }
    void element_prefix(bool& first) {
        if (!first) {
            comma();
        }
        first = false;
    }
    void node(std::string_view type, std::string_view summary, const std::function<void()>& fn) {
        begin_object();
        field("type", type, true);
        field("summary", summary, true);
        key("children");
        begin_array();
        fn();
        end_array();
        end_object();
    }
    void leaf(std::string_view type, std::string_view summary) { node(type, summary, [] {}); }

    void dump_package_clause(const PackageClause& package) {
        leaf("PackageClause",
             "name=" + package.name->string() + attributes_summary(package.attributes) +
                 location_summary(package.loc));
    }
    void dump_import(const ImportDeclaration& import) {
        std::string summary = "path=\"" + import.path->value + "\"";
        if (import.alias) {
            summary = "alias=" + import.alias->string() + ", " + summary;
        }
        summary += attributes_summary(import.attributes);
        summary += location_summary(import.loc);
        leaf("ImportDeclaration", summary);
    }
    void dump_statement(const Statement& stmt) {
        const std::string attrs = attributes_summary(stmt.attributes);
        const std::string loc = location_summary(stmt.loc);
        switch (stmt.type) {
            case Statement::Type::ExpressionStatement: {
                const auto& expr = std::get<std::unique_ptr<ExprStmt>>(stmt.stmt);
                node("ExpressionStatement", attrs + loc, [&] { dump_expression(*expr->expression); });
                break;
            }
            case Statement::Type::VariableAssignment: {
                const auto& assign = std::get<std::unique_ptr<VariableAssgn>>(stmt.stmt);
                node("VariableAssignment", "id=" + assign->id->string() + attrs + loc,
                     [&] { dump_expression(*assign->init); });
                break;
            }
            case Statement::Type::OptionStatement: {
                const auto& option = std::get<std::unique_ptr<OptionStmt>>(stmt.stmt);
                node("OptionStatement", attrs + loc, [&] { dump_assignment(*option->assignment); });
                break;
            }
            case Statement::Type::ReturnStatement: {
                const auto& ret = std::get<std::unique_ptr<ReturnStmt>>(stmt.stmt);
                node("ReturnStatement", attrs + loc, [&] { dump_expression(*ret->argument); });
                break;
            }
            case Statement::Type::BadStatement: {
                const auto& bad = std::get<std::unique_ptr<BadStmt>>(stmt.stmt);
                leaf("BadStatement", bad->text + attrs + loc);
                break;
            }
            case Statement::Type::TestCaseStatement: {
                const auto& testcase = std::get<std::unique_ptr<TestCaseStmt>>(stmt.stmt);
                std::string summary = "id=" + testcase->id->string() + attrs + loc;
                if (testcase->extends) {
                    summary += ", extends=\"" + testcase->extends->value + "\"";
                }
                node("TestCaseStatement", summary, [&] { dump_block(*testcase->block); });
                break;
            }
            case Statement::Type::BuiltinStatement: {
                const auto& builtin = std::get<std::unique_ptr<BuiltinStmt>>(stmt.stmt);
                node("BuiltinStatement", "id=" + builtin->id->string() + attrs + loc,
                     [&] { dump_type_expression(*builtin->ty); });
                break;
            }
        }
    }
    void dump_assignment(const Assignment& assignment) {
        switch (assignment.type) {
            case Assignment::Type::VariableAssignment: {
                const auto& assign = std::get<std::unique_ptr<VariableAssgn>>(assignment.value);
                node("VariableAssignment", "id=" + assign->id->string(),
                     [&] { dump_expression(*assign->init); });
                break;
            }
            case Assignment::Type::MemberAssignment: {
                const auto& assign = std::get<std::unique_ptr<MemberAssgn>>(assignment.value);
                node("MemberAssignment", "", [&] {
                    bool first = true;
                    element_prefix(first);
                    dump_member_expr(*assign->member);
                    element_prefix(first);
                    dump_expression(*assign->init);
                });
                break;
            }
        }
    }
    void dump_block(const Block& block) {
        node("Block", location_summary(block.loc), [&] {
            bool first = true;
            for (const auto& stmt : block.body) {
                element_prefix(first);
                dump_statement(*stmt);
            }
        });
    }
    void dump_property(const Property& property) {
        node("Property", "key=" + property.key->string(), [&] {
            if (property.value) {
                dump_expression(*property.value);
            }
        });
    }
    void dump_expression(const Expression& expr) {
        switch (expr.type) {
            case Expression::Type::Identifier:
                leaf("Identifier",
                     "name=" + std::get<std::unique_ptr<Identifier>>(expr.expr)->string());
                break;
            case Expression::Type::ArrayExpr:
                dump_array_expr(*std::get<std::unique_ptr<ArrayExpr>>(expr.expr));
                break;
            case Expression::Type::DictExpr:
                dump_dict_expr(*std::get<std::unique_ptr<DictExpr>>(expr.expr));
                break;
            case Expression::Type::FunctionExpr:
                dump_function_expr(*std::get<std::unique_ptr<FunctionExpr>>(expr.expr));
                break;
            case Expression::Type::LogicalExpr:
                dump_logical_expr(*std::get<std::unique_ptr<LogicalExpr>>(expr.expr));
                break;
            case Expression::Type::ObjectExpr:
                dump_object_expr(*std::get<std::unique_ptr<ObjectExpr>>(expr.expr));
                break;
            case Expression::Type::MemberExpr:
                dump_member_expr(*std::get<std::unique_ptr<MemberExpr>>(expr.expr));
                break;
            case Expression::Type::IndexExpr:
                dump_index_expr(*std::get<std::unique_ptr<IndexExpr>>(expr.expr));
                break;
            case Expression::Type::BinaryExpr:
                dump_binary_expr(*std::get<std::unique_ptr<BinaryExpr>>(expr.expr));
                break;
            case Expression::Type::UnaryExpr:
                dump_unary_expr(*std::get<std::unique_ptr<UnaryExpr>>(expr.expr));
                break;
            case Expression::Type::PipeExpr:
                dump_pipe_expr(*std::get<std::unique_ptr<PipeExpr>>(expr.expr));
                break;
            case Expression::Type::CallExpr:
                dump_call_expr(*std::get<std::unique_ptr<CallExpr>>(expr.expr));
                break;
            case Expression::Type::ConditionalExpr:
                dump_conditional_expr(*std::get<std::unique_ptr<ConditionalExpr>>(expr.expr));
                break;
            case Expression::Type::StringExpr:
                dump_string_expr(*std::get<std::unique_ptr<StringExpr>>(expr.expr));
                break;
            case Expression::Type::ParenExpr:
                node("ParenExpr", "", [&] {
                    dump_expression(*std::get<std::unique_ptr<ParenExpr>>(expr.expr)->expression);
                });
                break;
            case Expression::Type::IntegerLit:
                leaf("IntegerLit",
                     "value=" + std::get<std::unique_ptr<IntegerLit>>(expr.expr)->string());
                break;
            case Expression::Type::FloatLit:
                leaf("FloatLit",
                     "value=" + std::get<std::unique_ptr<FloatLit>>(expr.expr)->string());
                break;
            case Expression::Type::StringLit:
                leaf("StringLit",
                     "value=\"" + std::get<std::unique_ptr<StringLit>>(expr.expr)->value + "\"");
                break;
            case Expression::Type::DurationLit:
                leaf("DurationLit",
                     "value=" + std::get<std::unique_ptr<DurationLit>>(expr.expr)->string());
                break;
            case Expression::Type::UnsignedIntegerLit:
                leaf("UintLit",
                     "value=" + std::get<std::unique_ptr<UintLit>>(expr.expr)->string());
                break;
            case Expression::Type::BooleanLit:
                leaf("BooleanLit",
                     "value=" + std::get<std::unique_ptr<BooleanLit>>(expr.expr)->string());
                break;
            case Expression::Type::DateTimeLit:
                leaf("DateTimeLit",
                     "value=" + std::get<std::unique_ptr<DateTimeLit>>(expr.expr)->string());
                break;
            case Expression::Type::RegexpLit:
                leaf("RegexpLit",
                     "value=" + std::get<std::unique_ptr<RegexpLit>>(expr.expr)->string());
                break;
            case Expression::Type::PipeLit:
                leaf("PipeLit", "");
                break;
            case Expression::Type::LabelLit:
                leaf("LabelLit",
                     "value=" + std::get<std::unique_ptr<LabelLit>>(expr.expr)->string());
                break;
            case Expression::Type::BadExpr:
                leaf("BadExpr",
                     std::get<std::unique_ptr<BadExpr>>(expr.expr)->text +
                         location_summary(expr.loc));
                break;
        }
    }
    void dump_array_expr(const ArrayExpr& expr) {
        node("ArrayExpr", "", [&] {
            bool first = true;
            for (const auto& item : expr.elements) {
                element_prefix(first);
                dump_expression(*item->expression);
            }
        });
    }
    void dump_dict_expr(const DictExpr& expr) {
        node("DictExpr", "", [&] {
            bool first = true;
            for (const auto& item : expr.elements) {
                element_prefix(first);
                node("DictItem", "", [&] {
                    bool child_first = true;
                    element_prefix(child_first);
                    dump_expression(*item->key);
                    element_prefix(child_first);
                    dump_expression(*item->val);
                });
            }
        });
    }
    void dump_function_expr(const FunctionExpr& expr) {
        node("FunctionExpr", "", [&] {
            bool first = true;
            for (const auto& param : expr.params) {
                element_prefix(first);
                dump_property(*param);
            }
            if (expr.body->type == FunctionBody::Type::Block) {
                element_prefix(first);
                dump_block(*std::get<std::unique_ptr<Block>>(expr.body->body));
            } else {
                element_prefix(first);
                dump_expression(*std::get<std::unique_ptr<Expression>>(expr.body->body));
            }
        });
    }
    void dump_logical_expr(const LogicalExpr& expr) {
        node("LogicalExpr", "op=" + op_string(expr.op), [&] {
            bool first = true;
            element_prefix(first);
            dump_expression(*expr.left);
            element_prefix(first);
            dump_expression(*expr.right);
        });
    }
    void dump_object_expr(const ObjectExpr& expr) {
        std::string summary;
        if (expr.with) {
            summary = "with=" + expr.with->string();
        }
        node("ObjectExpr", summary, [&] {
            bool first = true;
            for (const auto& prop : expr.properties) {
                element_prefix(first);
                dump_property(*prop);
            }
        });
    }
    void dump_member_expr(const MemberExpr& expr) {
        node("MemberExpr", "property=" + expr.property->string(), [&] {
            dump_expression(*expr.object);
        });
    }
    void dump_index_expr(const IndexExpr& expr) {
        node("IndexExpr", "", [&] {
            bool first = true;
            element_prefix(first);
            dump_expression(*expr.array);
            element_prefix(first);
            dump_expression(*expr.index);
        });
    }
    void dump_binary_expr(const BinaryExpr& expr) {
        node("BinaryExpr", "op=" + op_string(expr.op), [&] {
            bool first = true;
            element_prefix(first);
            dump_expression(*expr.left);
            element_prefix(first);
            dump_expression(*expr.right);
        });
    }
    void dump_unary_expr(const UnaryExpr& expr) {
        node("UnaryExpr", "op=" + op_string(expr.op), [&] { dump_expression(*expr.argument); });
    }
    void dump_pipe_expr(const PipeExpr& expr) {
        node("PipeExpr", "", [&] {
            bool first = true;
            element_prefix(first);
            dump_expression(*expr.argument);
            element_prefix(first);
            dump_call_expr(*expr.call);
        });
    }
    void dump_call_expr(const CallExpr& expr) {
        node("CallExpr", "", [&] {
            bool first = true;
            element_prefix(first);
            dump_expression(*expr.callee);
            for (const auto& arg : expr.arguments) {
                element_prefix(first);
                dump_expression(*arg);
            }
        });
    }
    void dump_conditional_expr(const ConditionalExpr& expr) {
        node("ConditionalExpr", "", [&] {
            bool first = true;
            element_prefix(first);
            node("Test", "", [&] { dump_expression(*expr.test); });
            element_prefix(first);
            node("Consequent", "", [&] { dump_expression(*expr.consequent); });
            element_prefix(first);
            node("Alternate", "", [&] { dump_expression(*expr.alternate); });
        });
    }
    void dump_string_expr(const StringExpr& expr) {
        node("StringExpr", "", [&] {
            bool first = true;
            for (const auto& part : expr.parts) {
                element_prefix(first);
                switch (part->type) {
                    case StringExprPart::Type::Text:
                        leaf("TextPart", "value=\"" +
                                             std::get<std::unique_ptr<TextPart>>(part->part)->value +
                                             "\"");
                        break;
                    case StringExprPart::Type::Interpolated:
                        node("InterpolatedPart", "", [&] {
                            dump_expression(
                                *std::get<std::unique_ptr<InterpolatedPart>>(part->part)->expression);
                        });
                        break;
                }
            }
        });
    }
    void dump_type_expression(const TypeExpression& type_expr) {
        node("TypeExpression", "", [&] {
            bool first = true;
            element_prefix(first);
            dump_monotype(*type_expr.monotype);
            for (const auto& constraint : type_expr.constraints) {
                element_prefix(first);
                dump_type_constraint(*constraint);
            }
        });
    }
    void dump_type_constraint(const TypeConstraint& constraint) {
        std::string summary = "tvar=" + constraint.tvar->string();
        if (!constraint.kinds.empty()) {
            summary += ", kinds=";
            for (size_t i = 0; i < constraint.kinds.size(); ++i) {
                if (i != 0) {
                    summary += "+";
                }
                summary += constraint.kinds[i]->string();
            }
        }
        leaf("TypeConstraint", summary);
    }
    void dump_monotype(const MonoType& mono) {
        switch (mono.type) {
            case MonoType::Type::Tvar:
                leaf("MonoType::Tvar",
                     "name=" + std::get<std::unique_ptr<TvarType>>(mono.value)->name->string());
                break;
            case MonoType::Type::Basic:
                leaf("MonoType::Basic",
                     "name=" + std::get<std::unique_ptr<NamedType>>(mono.value)->name->string());
                break;
            case MonoType::Type::Array:
                node("MonoType::Array", "", [&] {
                    dump_monotype(*std::get<std::unique_ptr<ArrayType>>(mono.value)->element);
                });
                break;
            case MonoType::Type::Stream:
                node("MonoType::Stream", "", [&] {
                    dump_monotype(*std::get<std::unique_ptr<StreamType>>(mono.value)->element);
                });
                break;
            case MonoType::Type::Vector:
                node("MonoType::Vector", "", [&] {
                    dump_monotype(*std::get<std::unique_ptr<VectorType>>(mono.value)->element);
                });
                break;
            case MonoType::Type::Dict:
                node("MonoType::Dict", "", [&] {
                    bool first = true;
                    element_prefix(first);
                    dump_monotype(*std::get<std::unique_ptr<DictType>>(mono.value)->key);
                    element_prefix(first);
                    dump_monotype(*std::get<std::unique_ptr<DictType>>(mono.value)->val);
                });
                break;
            case MonoType::Type::Dynamic:
                leaf("MonoType::Dynamic", "");
                break;
            case MonoType::Type::Record: {
                const auto& record = *std::get<std::unique_ptr<RecordType>>(mono.value);
                std::string summary;
                if (record.tvar) {
                    summary = "with=" + record.tvar->string();
                }
                node("MonoType::Record", summary, [&] {
                    bool first = true;
                    for (const auto& prop : record.properties) {
                        element_prefix(first);
                        node("PropertyType", "name=" + prop->name->string(),
                             [&] { dump_monotype(*prop->monotype); });
                    }
                });
                break;
            }
            case MonoType::Type::Function: {
                const auto& function = *std::get<std::unique_ptr<FunctionType>>(mono.value);
                node("MonoType::Function", "", [&] {
                    bool first = true;
                    for (const auto& param : function.parameters) {
                        element_prefix(first);
                        switch (param->type) {
                            case ParameterType::Type::Required: {
                                const auto& required =
                                    std::get<std::shared_ptr<Required>>(param->value);
                                node("RequiredParam", "name=" + required->name->string(),
                                     [&] { dump_monotype(*required->monotype); });
                                break;
                            }
                            case ParameterType::Type::Optional: {
                                const auto& optional =
                                    std::get<std::unique_ptr<Optional>>(param->value);
                                node("OptionalParam", "name=" + optional->name->string(),
                                     [&] { dump_monotype(*optional->monotype); });
                                break;
                            }
                            case ParameterType::Type::Pipe: {
                                const auto& pipe = std::get<std::unique_ptr<Pipe>>(param->value);
                                node("PipeParam", "name=" + pipe->name->string(),
                                     [&] { dump_monotype(*pipe->monotype); });
                                break;
                            }
                        }
                    }
                    element_prefix(first);
                    dump_monotype(*function.monotype);
                });
                break;
            }
            case MonoType::Type::Label:
                leaf("MonoType::Label",
                     "value=" + std::get<std::unique_ptr<LabelLit>>(mono.value)->string());
                break;
        }
    }

    std::ostringstream out_;
};

} // namespace

std::string dump_ast(const File& file) { return AstDumper().dump(file); }

std::string dump_ast_json(const File& file) { return AstJsonDumper().dump(file); }

} // namespace pl
