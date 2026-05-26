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
// Created: 2026/05/24 11:04

#include "cpp/pl/flux/analysis/semantic_analyzer.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "cpp/pl/flux/analysis/builtin_metadata.h"

namespace pl::flux::analysis {
namespace {

bool position_in_range(uint32_t line, uint32_t column, const SourceLocation& loc) {
    if (line < loc.start.line || line > loc.end.line) {
        return false;
    }
    if (line == loc.start.line && column < loc.start.column) {
        return false;
    }
    if (line == loc.end.line && column > loc.end.column) {
        return false;
    }
    return true;
}

uint64_t location_span(const SourceLocation& loc) {
    const uint64_t line_span = static_cast<uint64_t>(loc.end.line - loc.start.line);
    const uint64_t col_span =
        loc.end.column >= loc.start.column ? loc.end.column - loc.start.column : 0;
    return line_span * 1000000ULL + col_span;
}

SourceLocation name_location(const SourceLocation& loc, std::string_view name) {
    SourceLocation result = loc;
    result.end.line = result.start.line;
    result.end.column = result.start.column + static_cast<uint32_t>(name.size());
    return result;
}

SourceLocation offset_name_location(const SourceLocation& loc,
                                    uint32_t offset,
                                    std::string_view name) {
    SourceLocation result = loc;
    result.start.column += offset;
    result.end.line = result.start.line;
    result.end.column = result.start.column + static_cast<uint32_t>(name.size());
    return result;
}

std::string property_name(const PropertyKey& key) {
    if (key.type == PropertyKey::Type::Identifier) {
        const auto& id = *std::get<std::unique_ptr<Identifier>>(key.key);
        return id.name;
    }
    const auto& str = *std::get<std::unique_ptr<StringLit>>(key.key);
    return str.value;
}

std::string import_binding_name(const ImportDeclaration& imp) {
    if (imp.alias && !imp.alias->name.empty()) {
        return imp.alias->name;
    }
    if (!imp.path) {
        return "";
    }
    const auto& path = imp.path->value;
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

SourceLocation import_binding_location(const ImportDeclaration& imp, std::string_view name) {
    if (imp.alias && !imp.alias->name.empty()) {
        return offset_name_location(imp.loc, 7, name);
    }
    if (imp.path) {
        const auto& path = imp.path->value;
        const auto name_offset = path.size() >= name.size() ? path.size() - name.size() : 0;
        return offset_name_location(imp.loc, 8 + static_cast<uint32_t>(name_offset), name);
    }
    return name_location(imp.loc, name);
}

bool is_named_call_argument(const Expression& expr) {
    if (expr.type != Expression::Type::ObjectExpr) {
        return false;
    }
    const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(expr.expr);
    return obj.with == nullptr;
}

size_t edit_distance(std::string_view lhs, std::string_view rhs) {
    std::vector<size_t> prev(rhs.size() + 1);
    std::vector<size_t> curr(rhs.size() + 1);
    for (size_t j = 0; j <= rhs.size(); ++j) {
        prev[j] = j;
    }
    for (size_t i = 1; i <= lhs.size(); ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= rhs.size(); ++j) {
            const size_t cost = lhs[i - 1] == rhs[j - 1] ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(curr);
    }
    return prev[rhs.size()];
}

std::optional<std::string> closest_name(std::string_view target,
                                        const std::vector<std::string>& candidates) {
    std::optional<std::string> best;
    size_t best_distance = std::numeric_limits<size_t>::max();
    for (const auto& candidate : candidates) {
        const auto distance = edit_distance(target, candidate);
        if (distance < best_distance) {
            best_distance = distance;
            best = candidate;
        }
    }
    if (best.has_value() && best_distance <= std::max<size_t>(2, target.size() / 3)) {
        return best;
    }
    return std::nullopt;
}

struct ResolvedCallee {
    const BuiltinSignature* sig = nullptr;
    SourceLocation location;
    size_t package_definition_id = 0;
    std::string package;
    std::string member;
};

struct FunctionContext {
    std::unordered_map<std::string, Type> params;
    std::optional<Type> return_type;
};

RecordFieldType make_field(std::string name, Type type) {
    return {.name = std::move(name), .type = std::make_shared<Type>(std::move(type))};
}

std::vector<std::string> split_csv_record(std::string_view line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(ch);
            }
            continue;
        }
        if (ch == ',') {
            fields.push_back(field);
            field.clear();
        } else if (ch == '"') {
            if (field.empty()) {
                quoted = true;
            } else {
                field.push_back(ch);
            }
        } else {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}

Type type_for_csv_datatype(std::string_view datatype) {
    if (datatype == "long") {
        return Type::Scalar(TypeKind::Int);
    }
    if (datatype == "unsignedLong") {
        return Type::Scalar(TypeKind::UInt);
    }
    if (datatype == "double") {
        return Type::Scalar(TypeKind::Float);
    }
    if (datatype == "boolean" || datatype == "bool") {
        return Type::Scalar(TypeKind::Bool);
    }
    if (datatype == "dateTime:RFC3339" || datatype == "dateTime:RFC3339Nano") {
        return Type::Scalar(TypeKind::Time);
    }
    if (datatype == "duration") {
        return Type::Scalar(TypeKind::Duration);
    }
    return Type::Scalar(TypeKind::String);
}

std::optional<std::string> read_text_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::optional<Type> infer_csv_schema_from_text(const std::string& csv, std::string_view mode) {
    std::istringstream input(csv);
    std::string line;
    std::vector<std::string> datatypes;
    std::vector<std::string> header;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto fields = split_csv_record(line);
        if (fields.empty()) {
            continue;
        }
        if (mode != "raw" && fields[0] == "#datatype") {
            datatypes = std::move(fields);
            continue;
        }
        if (mode != "raw" && (fields[0] == "#group" || fields[0] == "#default")) {
            continue;
        }
        header = std::move(fields);
        break;
    }

    if (header.empty()) {
        return std::nullopt;
    }

    std::vector<RecordFieldType> fields;
    fields.reserve(header.size());
    for (size_t i = 0; i < header.size(); ++i) {
        if (header[i].empty()) {
            continue;
        }
        auto type = Type::Scalar(TypeKind::String);
        if (mode != "raw" && i < datatypes.size()) {
            type = type_for_csv_datatype(datatypes[i]);
        }
        fields.push_back(make_field(header[i], std::move(type)));
    }
    return Type::Stream(Type::Record(std::move(fields), false));
}

class SourceSchemaResolver {
public:
    std::optional<Type> ResolveCsv(std::optional<std::string> csv,
                                   std::optional<std::string> file,
                                   std::optional<std::string> mode) const {
        const std::string resolved_mode = mode.value_or("annotations");
        if (csv.has_value()) {
            return infer_csv_schema_from_text(*csv, resolved_mode);
        }
        if (file.has_value()) {
            auto text = read_text_file(*file);
            if (text.has_value()) {
                return infer_csv_schema_from_text(*text, resolved_mode);
            }
        }
        return std::nullopt;
    }
};

class Binder {
public:
    AnalysisResult Bind(const File& file) {
        push_scope(file.loc);
        bind_imports(file);
        predeclare_top_level_bindings(file);
        for (const auto& stmt : file.body) {
            if (stmt) {
                visit_statement(*stmt);
            }
        }
        pop_scope();
        return std::move(result_);
    }

private:
    struct Scope {
        size_t id = 0;
        size_t parent_id = 0;
        std::unordered_map<std::string, size_t> definitions;
    };

    void bind_imports(const File& file) {
        for (const auto& imp : file.imports) {
            if (!imp || !imp->path) {
                continue;
            }
            const auto name = import_binding_name(*imp);
            if (name.empty()) {
                continue;
            }
            auto loc = import_binding_location(*imp, name);
            const auto id =
                define(name, SymbolKind::Import, loc, {}, imp->path->value, std::nullopt);
            imports_[name] = {.path = imp->path->value, .definition_id = id};
            result_.imported_packages.push_back(imp->path->value);
        }
    }

    void predeclare_top_level_bindings(const File& file) {
        for (const auto& stmt : file.body) {
            if (!stmt) {
                continue;
            }
            switch (stmt->type) {
                case Statement::Type::VariableAssignment: {
                    const auto& va = *std::get<std::unique_ptr<VariableAssgn>>(stmt->stmt);
                    if (va.id && !va.id->name.empty()) {
                        predeclare(va.id->name,
                                   SymbolKind::Variable,
                                   name_location(stmt->loc, va.id->name));
                    }
                    break;
                }
                case Statement::Type::BuiltinStatement: {
                    const auto& bi = *std::get<std::unique_ptr<BuiltinStmt>>(stmt->stmt);
                    if (bi.id && !bi.id->name.empty()) {
                        const uint32_t offset =
                            stmt->loc.start.column + 8 <= stmt->loc.end.column ? 8U : 0U;
                        predeclare(bi.id->name,
                                   SymbolKind::Builtin,
                                   offset_name_location(stmt->loc, offset, bi.id->name),
                                   Type::Dynamic());
                    }
                    break;
                }
                case Statement::Type::TestCaseStatement: {
                    const auto& tc = *std::get<std::unique_ptr<TestCaseStmt>>(stmt->stmt);
                    if (tc.id && !tc.id->name.empty()) {
                        const uint32_t offset =
                            stmt->loc.start.column + 9 <= stmt->loc.end.column ? 9U : 0U;
                        predeclare(tc.id->name,
                                   SymbolKind::Function,
                                   offset_name_location(stmt->loc, offset, tc.id->name));
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    void visit_statement(const Statement& stmt) {
        switch (stmt.type) {
            case Statement::Type::VariableAssignment:
                visit_variable_assignment(*std::get<std::unique_ptr<VariableAssgn>>(stmt.stmt),
                                          stmt.loc);
                break;
            case Statement::Type::OptionStatement:
                visit_option_statement(*std::get<std::unique_ptr<OptionStmt>>(stmt.stmt), stmt.loc);
                break;
            case Statement::Type::BuiltinStatement:
                visit_builtin_statement(*std::get<std::unique_ptr<BuiltinStmt>>(stmt.stmt),
                                        stmt.loc);
                break;
            case Statement::Type::ExpressionStatement: {
                const auto& es = *std::get<std::unique_ptr<ExprStmt>>(stmt.stmt);
                if (es.expression) {
                    visit_expression(*es.expression);
                }
                break;
            }
            case Statement::Type::ReturnStatement: {
                const auto& ret = *std::get<std::unique_ptr<ReturnStmt>>(stmt.stmt);
                if (ret.argument) {
                    visit_expression(*ret.argument);
                }
                break;
            }
            case Statement::Type::TestCaseStatement:
                visit_testcase_statement(*std::get<std::unique_ptr<TestCaseStmt>>(stmt.stmt),
                                         stmt.loc);
                break;
            case Statement::Type::BadStatement:
                break;
        }
    }

    void visit_variable_assignment(const VariableAssgn& va, const SourceLocation& loc) {
        auto init_type = va.init ? visit_expression(*va.init) : Type::Unknown();
        if (va.id && !va.id->name.empty()) {
            auto params =
                va.init && va.init->type == Expression::Type::FunctionExpr
                    ? function_param_names(*std::get<std::unique_ptr<FunctionExpr>>(va.init->expr))
                    : std::vector<std::string>{};
            const auto kind = va.init && va.init->type == Expression::Type::FunctionExpr
                                  ? SymbolKind::Function
                                  : SymbolKind::Variable;
            if (update_predeclared(va.id->name, kind, std::move(params), std::move(init_type))) {
                return;
            }
            define(va.id->name,
                   kind,
                   name_location(loc, va.id->name),
                   std::move(params),
                   std::nullopt,
                   std::nullopt,
                   std::move(init_type));
        }
    }

    void visit_option_statement(const OptionStmt& opt, const SourceLocation& loc) {
        if (!opt.assignment) {
            return;
        }
        if (opt.assignment->type == Assignment::Type::VariableAssignment) {
            const auto& va = *std::get<std::unique_ptr<VariableAssgn>>(opt.assignment->value);
            auto init_type = va.init ? visit_expression(*va.init) : Type::Unknown();
            if (va.id && !va.id->name.empty()) {
                define(va.id->name,
                       SymbolKind::Option,
                       name_location(loc, va.id->name),
                       {},
                       std::nullopt,
                       std::nullopt,
                       std::move(init_type));
            }
            return;
        }

        const auto& ma = *std::get<std::unique_ptr<MemberAssgn>>(opt.assignment->value);
        if (ma.member) {
            visit_member_assignment_target(*ma.member);
        }
        if (ma.init) {
            (void)visit_expression(*ma.init);
        }
    }

    void visit_builtin_statement(const BuiltinStmt& bi, const SourceLocation& loc) {
        if (!bi.id || bi.id->name.empty()) {
            return;
        }
        if (update_predeclared(bi.id->name, SymbolKind::Builtin, {}, Type::Dynamic())) {
            return;
        }
        const uint32_t offset = loc.start.column + 8 <= loc.end.column ? 8U : 0U;
        define(bi.id->name,
               SymbolKind::Builtin,
               offset_name_location(loc, offset, bi.id->name),
               {},
               std::nullopt,
               "",
               Type::Dynamic());
    }

    void visit_testcase_statement(const TestCaseStmt& tc, const SourceLocation& loc) {
        if (tc.id && !tc.id->name.empty()) {
            if (update_predeclared(tc.id->name, SymbolKind::Function, {}, Type::Unknown())) {
                if (tc.block) {
                    visit_block(*tc.block);
                }
                return;
            }
            const uint32_t offset = loc.start.column + 9 <= loc.end.column ? 9U : 0U;
            define(
                tc.id->name, SymbolKind::Function, offset_name_location(loc, offset, tc.id->name));
        }
        if (tc.block) {
            visit_block(*tc.block);
        }
    }

    void visit_block(const Block& block) {
        push_scope(block.loc);
        for (const auto& stmt : block.body) {
            if (stmt) {
                visit_statement(*stmt);
            }
        }
        pop_scope();
    }

    Type visit_expression(const Expression& expr) {
        Type type = Type::Unknown();
        switch (expr.type) {
            case Expression::Type::Identifier:
                type = visit_identifier_expression(expr, ReferenceKind::Identifier);
                break;
            case Expression::Type::CallExpr:
                type = visit_call_expression(
                    *std::get<std::unique_ptr<CallExpr>>(expr.expr), false, Type::Unknown());
                break;
            case Expression::Type::PipeExpr:
                type = visit_pipe_expression(*std::get<std::unique_ptr<PipeExpr>>(expr.expr));
                break;
            case Expression::Type::MemberExpr:
                type = visit_member_expression(*std::get<std::unique_ptr<MemberExpr>>(expr.expr),
                                               expr.loc);
                break;
            case Expression::Type::FunctionExpr:
                type = visit_function_expression(
                    *std::get<std::unique_ptr<FunctionExpr>>(expr.expr), expr.loc, nullptr);
                break;
            case Expression::Type::ObjectExpr:
                type = visit_object_expression(*std::get<std::unique_ptr<ObjectExpr>>(expr.expr),
                                               expr.loc);
                break;
            case Expression::Type::ArrayExpr:
                type = visit_array_expression(*std::get<std::unique_ptr<ArrayExpr>>(expr.expr));
                break;
            case Expression::Type::DictExpr:
                type = visit_dict_expression(*std::get<std::unique_ptr<DictExpr>>(expr.expr));
                break;
            case Expression::Type::IndexExpr:
                type = visit_index_expression(*std::get<std::unique_ptr<IndexExpr>>(expr.expr));
                break;
            case Expression::Type::BinaryExpr:
                type = visit_binary_expression(*std::get<std::unique_ptr<BinaryExpr>>(expr.expr),
                                               expr.loc);
                break;
            case Expression::Type::LogicalExpr:
                type = visit_logical_expression(*std::get<std::unique_ptr<LogicalExpr>>(expr.expr),
                                                expr.loc);
                break;
            case Expression::Type::UnaryExpr:
                type = visit_unary_expression(*std::get<std::unique_ptr<UnaryExpr>>(expr.expr),
                                              expr.loc);
                break;
            case Expression::Type::ConditionalExpr:
                type = visit_conditional_expression(
                    *std::get<std::unique_ptr<ConditionalExpr>>(expr.expr), expr.loc);
                break;
            case Expression::Type::ParenExpr: {
                const auto& paren = *std::get<std::unique_ptr<ParenExpr>>(expr.expr);
                if (paren.expression) {
                    type = visit_expression(*paren.expression);
                }
                break;
            }
            case Expression::Type::StringExpr:
                type = visit_string_expression(*std::get<std::unique_ptr<StringExpr>>(expr.expr));
                break;
            case Expression::Type::IntegerLit:
                type = Type::Scalar(TypeKind::Int);
                break;
            case Expression::Type::FloatLit:
                type = Type::Scalar(TypeKind::Float);
                break;
            case Expression::Type::StringLit:
                type = Type::Scalar(TypeKind::String);
                break;
            case Expression::Type::DurationLit:
                type = Type::Scalar(TypeKind::Duration);
                break;
            case Expression::Type::UnsignedIntegerLit:
                type = Type::Scalar(TypeKind::UInt);
                break;
            case Expression::Type::BooleanLit:
                type = Type::Scalar(TypeKind::Bool);
                break;
            case Expression::Type::DateTimeLit:
                type = Type::Scalar(TypeKind::Time);
                break;
            case Expression::Type::RegexpLit:
                type = Type::Scalar(TypeKind::Regexp);
                break;
            case Expression::Type::LabelLit:
                type = Type::Scalar(TypeKind::Label);
                break;
            case Expression::Type::PipeLit:
                type = Type::Dynamic();
                break;
            default:
                break;
        }
        add_expression(expr.loc, type);
        return type;
    }

    Type visit_identifier_expression(const Expression& expr, ReferenceKind kind) {
        const auto& id = *std::get<std::unique_ptr<Identifier>>(expr.expr);
        if (!id.name.empty()) {
            (void)reference(id.name, expr.loc, kind);
            auto type = type_for_name(id.name);
            return type;
        }
        return Type::Unknown();
    }

    Type visit_call_expression(const CallExpr& call, bool pipe_value_present, Type pipe_type) {
        const auto resolved = resolve_callee(call);
        if (resolved.sig != nullptr && IsCallableBuiltin(*resolved.sig)) {
            check_builtin_call(call, *resolved.sig, resolved.location, pipe_value_present);
        }
        visit_callee(call.callee.get(), resolved);
        std::vector<Type> arg_types;
        if (resolved.sig != nullptr && call.arguments.size() == 1 && call.arguments[0] &&
            is_named_call_argument(*call.arguments[0])) {
            const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(call.arguments[0]->expr);
            arg_types.push_back(visit_named_call_object(
                obj, call.arguments[0]->loc, *resolved.sig, pipe_value_present, pipe_type));
        } else {
            for (const auto& arg : call.arguments) {
                if (arg) {
                    arg_types.push_back(visit_expression(*arg));
                }
            }
        }
        if (resolved.sig != nullptr && IsCallableBuiltin(*resolved.sig)) {
            check_builtin_argument_types(
                call, *resolved.sig, arg_types, pipe_value_present, pipe_type);
        }
        auto return_type =
            infer_call_return_type(call, resolved, pipe_value_present, pipe_type, arg_types);
        result_.calls.push_back({
            .location = call.callee ? call.callee->loc : SourceLocation{},
            .callee_location = call.callee ? call.callee->loc : SourceLocation{},
            .callee = resolved.sig != nullptr ? resolved.sig->fq_name : callee_name(call),
            .package = resolved.package.empty() ? std::nullopt
                                                : std::optional<std::string>(resolved.package),
            .member = resolved.member.empty() ? std::nullopt
                                              : std::optional<std::string>(resolved.member),
            .argument_names = call_argument_names(call),
            .return_type = return_type,
            .pipe_value_present = pipe_value_present,
            .builtin = resolved.sig != nullptr,
        });
        return return_type;
    }

    Type visit_pipe_expression(const PipeExpr& pipe) {
        auto pipe_type = Type::Unknown();
        if (pipe.argument) {
            pipe_type = visit_expression(*pipe.argument);
        }
        if (pipe.call) {
            return visit_call_expression(*pipe.call, true, pipe_type);
        }
        return pipe_type;
    }

    void visit_callee(const Expression* callee, const ResolvedCallee& resolved) {
        if (callee == nullptr) {
            return;
        }
        if (callee->type == Expression::Type::Identifier) {
            const auto& id = *std::get<std::unique_ptr<Identifier>>(callee->expr);
            if (FindUniverseBuiltinSignature(id.name) != nullptr) {
                (void)reference(id.name, callee->loc, ReferenceKind::Callee);
            } else {
                (void)reference(id.name, callee->loc, ReferenceKind::Callee);
            }
            return;
        }
        if (callee->type == Expression::Type::MemberExpr && resolved.sig != nullptr) {
            const auto& mem = *std::get<std::unique_ptr<MemberExpr>>(callee->expr);
            if (mem.object != nullptr && mem.object->type == Expression::Type::Identifier) {
                const auto& obj = *std::get<std::unique_ptr<Identifier>>(mem.object->expr);
                (void)reference(obj.name, mem.object->loc, ReferenceKind::PackageObject);
            }
            result_.references.push_back({
                .name = resolved.sig->fq_name,
                .location = callee->loc,
                .kind = ReferenceKind::PackageMember,
                .scope_id = current_scope_id_,
                .definition_id = 0,
                .package = resolved.package,
                .member = resolved.member,
                .resolved = true,
            });
            return;
        }
        (void)visit_expression(*callee);
    }

    Type visit_member_expression(const MemberExpr& mem, const SourceLocation& loc) {
        if (!mem.object) {
            return Type::Unknown();
        }
        if (mem.object->type != Expression::Type::Identifier || !mem.property) {
            auto object_type = visit_expression(*mem.object);
            if (mem.property) {
                auto member = property_name(*mem.property);
                if (auto field = object_type.Field(member); field.has_value()) {
                    return *field;
                }
                if (object_type.kind == TypeKind::Record && !object_type.open_record) {
                    diagnostic("unknown field `" + member + "` on record " + object_type.ToString(),
                               loc,
                               DiagnosticSeverity::Warning);
                }
            }
            return Type::Dynamic();
        }
        const auto& obj = *std::get<std::unique_ptr<Identifier>>(mem.object->expr);
        const auto import_it = imports_.find(obj.name);
        if (import_it == imports_.end()) {
            const auto object_type =
                visit_identifier_expression(*mem.object, ReferenceKind::Identifier);
            if (mem.property) {
                auto member = property_name(*mem.property);
                if (auto field = object_type.Field(member); field.has_value()) {
                    return *field;
                }
            }
            return Type::Dynamic();
        }
        const auto member = property_name(*mem.property);
        (void)reference(obj.name, mem.object->loc, ReferenceKind::PackageObject);
        const auto* sig = FindBuiltinSignature(import_it->second.path, member);
        if (sig == nullptr && IsKnownPackage(import_it->second.path)) {
            diagnostic(unknown_package_member_message(import_it->second.path, member),
                       loc,
                       DiagnosticSeverity::Error);
            return Type::Error();
        }
        if (sig != nullptr) {
            result_.references.push_back({
                .name = sig->fq_name,
                .location = loc,
                .kind = ReferenceKind::PackageMember,
                .scope_id = current_scope_id_,
                .definition_id = 0,
                .package = import_it->second.path,
                .member = member,
                .resolved = true,
            });
            return ParseTypeExpression(sig->return_type);
        }
        return Type::Dynamic();
    }

    void visit_member_assignment_target(const MemberExpr& mem) {
        if (mem.object) {
            (void)visit_expression(*mem.object);
        }
    }

    Type visit_function_expression(const FunctionExpr& fn,
                                   const SourceLocation& loc,
                                   const FunctionContext* context) {
        push_scope(loc);
        std::unordered_set<std::string> seen;
        std::vector<FunctionParamType> params;
        for (const auto& param : fn.params) {
            if (!param || !param->key) {
                continue;
            }
            const auto name = property_name(*param->key);
            if (name.empty()) {
                continue;
            }
            if (!seen.insert(name).second) {
                diagnostic(
                    "duplicate function parameter: " + name, param->loc, DiagnosticSeverity::Error);
            }
            auto default_type = param->value ? visit_expression(*param->value) : Type::Dynamic();
            if (context != nullptr) {
                const auto context_it = context->params.find(name);
                if (context_it != context->params.end()) {
                    default_type = context_it->second;
                }
            }
            define(name,
                   SymbolKind::Parameter,
                   param->loc,
                   {},
                   std::nullopt,
                   std::nullopt,
                   default_type);
            params.push_back({.name = name,
                              .type = std::make_shared<Type>(default_type),
                              .optional = param->value != nullptr});
            if (param->value) {
                // Already visited to infer the default parameter type.
            }
        }
        Type result_type = Type::Unknown();
        if (fn.body) {
            if (fn.body->type == FunctionBody::Type::Block) {
                visit_block(*std::get<std::unique_ptr<Block>>(fn.body->body));
                result_type = Type::Dynamic();
            } else {
                result_type =
                    visit_expression(*std::get<std::unique_ptr<Expression>>(fn.body->body));
            }
        }
        if (context != nullptr && context->return_type.has_value() &&
            !CanAssign(*context->return_type, result_type)) {
            diagnostic("function result expects " + context->return_type->ToString() + ", got " +
                           result_type.ToString(),
                       loc,
                       DiagnosticSeverity::Warning);
        }
        pop_scope();
        return Type::Function(std::move(params), std::move(result_type));
    }

    Type visit_named_call_object(const ObjectExpr& obj,
                                 const SourceLocation& loc,
                                 const BuiltinSignature& sig,
                                 bool pipe_value_present,
                                 const Type& pipe_type) {
        std::vector<RecordFieldType> fields;
        for (const auto& prop : obj.properties) {
            if (!prop || !prop->key || !prop->value) {
                continue;
            }
            const auto name = property_name(*prop->key);
            auto context = function_context_for_argument(sig, name, pipe_value_present, pipe_type);
            Type value_type;
            if (context.has_value() && prop->value->type == Expression::Type::FunctionExpr) {
                value_type = visit_function_expression(
                    *std::get<std::unique_ptr<FunctionExpr>>(prop->value->expr),
                    prop->value->loc,
                    &*context);
                check_contextual_function_result(sig, name, value_type, prop->value->loc);
            } else {
                value_type = visit_expression(*prop->value);
            }
            upsert_field(fields, name, std::move(value_type));
        }
        result_.record_schemas.push_back({.location = loc, .fields = fields, .open = false});
        return Type::Record(std::move(fields), false);
    }

    std::optional<FunctionContext> function_context_for_argument(const BuiltinSignature& sig,
                                                                 std::string_view arg_name,
                                                                 bool pipe_value_present,
                                                                 const Type& pipe_type) const {
        if (!pipe_value_present || arg_name != "fn") {
            return std::nullopt;
        }
        const auto row = stream_row(pipe_type);
        if (!row.has_value()) {
            return std::nullopt;
        }
        if (sig.package.empty() && sig.name == "filter") {
            return FunctionContext{.params = {{"r", *row}},
                                   .return_type = Type::Scalar(TypeKind::Bool)};
        }
        if (sig.package.empty() && sig.name == "map") {
            return FunctionContext{.params = {{"r", *row}}, .return_type = std::nullopt};
        }
        if (sig.package.empty() && (sig.name == "findColumn" || sig.name == "findRecord")) {
            return FunctionContext{.params = {{"key", Type::Record({}, true)}},
                                   .return_type = Type::Scalar(TypeKind::Bool)};
        }
        return std::nullopt;
    }

    void check_contextual_function_result(const BuiltinSignature& sig,
                                          std::string_view arg_name,
                                          const Type& fn_type,
                                          const SourceLocation& loc) {
        if (arg_name != "fn" || fn_type.kind != TypeKind::Function || fn_type.args.empty()) {
            return;
        }
        const auto& result_type = fn_type.args[0];
        if (sig.package.empty() && sig.name == "map" && !result_type.IsUnknownLike() &&
            result_type.kind != TypeKind::Record) {
            diagnostic("map fn must return a record, got " + result_type.ToString(),
                       loc,
                       DiagnosticSeverity::Warning);
        }
    }

    Type visit_object_expression(const ObjectExpr& obj, const SourceLocation& loc) {
        std::vector<RecordFieldType> fields;
        bool open = false;
        if (obj.with && obj.with->source && !obj.with->source->name.empty()) {
            (void)reference(obj.with->source->name, loc, ReferenceKind::WithSource);
            auto source_type = type_for_name(obj.with->source->name);
            if (source_type.kind == TypeKind::Record) {
                fields = source_type.fields;
                open = source_type.open_record;
            } else {
                open = true;
            }
        }
        for (const auto& prop : obj.properties) {
            if (prop && prop->value) {
                auto value_type = visit_expression(*prop->value);
                if (prop->key) {
                    upsert_field(fields, property_name(*prop->key), std::move(value_type));
                }
            }
        }
        result_.record_schemas.push_back({.location = loc, .fields = fields, .open = open});
        return Type::Record(std::move(fields), open);
    }

    Type visit_array_expression(const ArrayExpr& arr) {
        auto element_type = Type::Unknown();
        for (const auto& item : arr.elements) {
            if (item && item->expression) {
                element_type = CommonType(element_type, visit_expression(*item->expression));
            }
        }
        return Type::Array(std::move(element_type));
    }

    Type visit_dict_expression(const DictExpr& dict) {
        auto key_type = Type::Unknown();
        auto value_type = Type::Unknown();
        for (const auto& item : dict.elements) {
            if (item && item->key) {
                key_type = CommonType(key_type, visit_expression(*item->key));
            }
            if (item && item->val) {
                value_type = CommonType(value_type, visit_expression(*item->val));
            }
        }
        return Type::Dict(std::move(key_type), std::move(value_type));
    }

    Type visit_index_expression(const IndexExpr& idx) {
        auto array_type = Type::Unknown();
        if (idx.array) {
            array_type = visit_expression(*idx.array);
        }
        if (idx.index) {
            (void)visit_expression(*idx.index);
        }
        if ((array_type.kind == TypeKind::Array || array_type.kind == TypeKind::Stream ||
             array_type.kind == TypeKind::Table) &&
            !array_type.args.empty()) {
            return array_type.args[0];
        }
        if (array_type.kind == TypeKind::Dict && array_type.args.size() > 1) {
            return array_type.args[1];
        }
        return Type::Dynamic();
    }

    Type visit_binary_expression(const BinaryExpr& bin, const SourceLocation& loc) {
        auto left = bin.left ? visit_expression(*bin.left) : Type::Unknown();
        auto right = bin.right ? visit_expression(*bin.right) : Type::Unknown();
        return infer_binary_type(bin.op, left, right, loc);
    }

    Type visit_logical_expression(const LogicalExpr& log, const SourceLocation& loc) {
        auto left = log.left ? visit_expression(*log.left) : Type::Unknown();
        auto right = log.right ? visit_expression(*log.right) : Type::Unknown();
        check_bool(left, loc, "logical expression");
        check_bool(right, loc, "logical expression");
        return Type::Scalar(TypeKind::Bool);
    }

    Type visit_unary_expression(const UnaryExpr& un, const SourceLocation& loc) {
        auto arg = un.argument ? visit_expression(*un.argument) : Type::Unknown();
        if (un.op == Operator::NotOperator) {
            check_bool(arg, loc, "`not`");
            return Type::Scalar(TypeKind::Bool);
        }
        if (un.op == Operator::ExistsOperator || un.op == Operator::EmptyOperator ||
            un.op == Operator::NotEmptyOperator) {
            return Type::Scalar(TypeKind::Bool);
        }
        if (!arg.IsUnknownLike() && !arg.IsNumeric() && arg.kind != TypeKind::Duration) {
            diagnostic("operator `" + op_string(un.op) +
                           "` expects a numeric or duration value, got " + arg.ToString(),
                       loc,
                       DiagnosticSeverity::Warning);
        }
        return arg;
    }

    Type visit_conditional_expression(const ConditionalExpr& cond, const SourceLocation& loc) {
        auto test = cond.test ? visit_expression(*cond.test) : Type::Unknown();
        check_bool(test, loc, "if condition");
        auto consequent = cond.consequent ? visit_expression(*cond.consequent) : Type::Unknown();
        auto alternate = cond.alternate ? visit_expression(*cond.alternate) : Type::Unknown();
        auto result = CommonType(consequent, alternate);
        if (result.kind == TypeKind::Dynamic && !consequent.IsUnknownLike() &&
            !alternate.IsUnknownLike()) {
            diagnostic("conditional branches have incompatible types: " + consequent.ToString() +
                           " and " + alternate.ToString(),
                       loc,
                       DiagnosticSeverity::Warning);
        }
        return result;
    }

    Type visit_string_expression(const StringExpr& str) {
        for (const auto& part : str.parts) {
            if (!part || part->type != StringExprPart::Type::Interpolated) {
                continue;
            }
            const auto& interpolated = *std::get<std::unique_ptr<InterpolatedPart>>(part->part);
            if (interpolated.expression) {
                (void)visit_expression(*interpolated.expression);
            }
        }
        return Type::Scalar(TypeKind::String);
    }

    void add_expression(const SourceLocation& loc, const Type& type) {
        result_.expressions.push_back(
            {.location = loc, .type = type, .reference_index = std::nullopt});
    }

    Type type_for_name(const std::string& name) const {
        if (name == "true" || name == "false") {
            return Type::Scalar(TypeKind::Bool);
        }
        if (name == "nothing") {
            return Type::Scalar(TypeKind::Null);
        }
        if (const auto id = resolve(name); id != 0) {
            for (const auto& def : result_.definitions) {
                if (def.id == id) {
                    return def.type;
                }
            }
        }
        if (const auto* sig = FindUniverseBuiltinSignature(name); sig != nullptr) {
            if (!IsCallableBuiltin(*sig)) {
                return ParseTypeExpression(sig->return_type);
            }
            return builtin_function_type(*sig);
        }
        return Type::Dynamic();
    }

    Type builtin_function_type(const BuiltinSignature& sig) const {
        std::vector<FunctionParamType> params;
        for (const auto& param : sig.params) {
            params.push_back({
                .name = param.name,
                .type = std::make_shared<Type>(ParseTypeExpression(param.type)),
                .optional = param.kind == BuiltinParamKind::Optional,
                .pipe = param.kind == BuiltinParamKind::Pipe,
            });
        }
        return Type::Function(std::move(params), ParseTypeExpression(sig.return_type));
    }

    void upsert_field(std::vector<RecordFieldType>& fields, std::string name, Type type) const {
        for (auto& field : fields) {
            if (field.name == name) {
                field.type = std::make_shared<Type>(std::move(type));
                return;
            }
        }
        fields.push_back(
            {.name = std::move(name), .type = std::make_shared<Type>(std::move(type))});
    }

    Type infer_binary_type(Operator op,
                           const Type& left,
                           const Type& right,
                           const SourceLocation& loc) {
        switch (op) {
            case Operator::LessThanEqualOperator:
            case Operator::LessThanOperator:
            case Operator::GreaterThanEqualOperator:
            case Operator::GreaterThanOperator:
            case Operator::EqualOperator:
            case Operator::NotEqualOperator:
            case Operator::InOperator:
                return Type::Scalar(TypeKind::Bool);
            case Operator::RegexpMatchOperator:
            case Operator::NotRegexpMatchOperator:
                if (!left.IsUnknownLike() && !left.IsString()) {
                    diagnostic("operator `" + op_string(op) +
                                   "` expects a string value on the left, got " + left.ToString(),
                               loc,
                               DiagnosticSeverity::Warning);
                }
                if (!right.IsUnknownLike() && right.kind != TypeKind::Regexp &&
                    right.kind != TypeKind::String) {
                    diagnostic("operator `" + op_string(op) +
                                   "` expects a regexp or string value on the right, got " +
                                   right.ToString(),
                               loc,
                               DiagnosticSeverity::Warning);
                }
                return Type::Scalar(TypeKind::Bool);
            case Operator::StartsWithOperator:
                if (!left.IsUnknownLike() && !left.IsString()) {
                    diagnostic("operator `startswith` expects string values",
                               loc,
                               DiagnosticSeverity::Warning);
                }
                return Type::Scalar(TypeKind::Bool);
            case Operator::AdditionOperator:
                if (left.kind == TypeKind::String && right.kind == TypeKind::String) {
                    return Type::Scalar(TypeKind::String);
                }
                if (left.kind == TypeKind::Duration && right.kind == TypeKind::Duration) {
                    return Type::Scalar(TypeKind::Duration);
                }
                [[fallthrough]];
            case Operator::SubtractionOperator:
            case Operator::MultiplicationOperator:
            case Operator::DivisionOperator:
            case Operator::ModuloOperator:
            case Operator::PowerOperator:
                if (!left.IsUnknownLike() && !left.IsNumeric() && left.kind != TypeKind::Duration) {
                    diagnostic("operator `" + op_string(op) + "` expects numeric operands, got " +
                                   left.ToString(),
                               loc,
                               DiagnosticSeverity::Warning);
                }
                if (!right.IsUnknownLike() && !right.IsNumeric() &&
                    right.kind != TypeKind::Duration) {
                    diagnostic("operator `" + op_string(op) + "` expects numeric operands, got " +
                                   right.ToString(),
                               loc,
                               DiagnosticSeverity::Warning);
                }
                return CommonType(left, right);
            default:
                return Type::Dynamic();
        }
    }

    void check_bool(const Type& type, const SourceLocation& loc, std::string_view context) {
        if (!type.IsUnknownLike() && !type.IsBool()) {
            diagnostic(std::string(context) + " expects bool, got " + type.ToString(),
                       loc,
                       DiagnosticSeverity::Warning);
        }
    }

    std::string callee_name(const CallExpr& call) const {
        if (!call.callee) {
            return "";
        }
        if (call.callee->type == Expression::Type::Identifier) {
            return std::get<std::unique_ptr<Identifier>>(call.callee->expr)->name;
        }
        if (call.callee->type == Expression::Type::MemberExpr) {
            const auto& mem = *std::get<std::unique_ptr<MemberExpr>>(call.callee->expr);
            if (mem.object && mem.object->type == Expression::Type::Identifier && mem.property) {
                return std::get<std::unique_ptr<Identifier>>(mem.object->expr)->name + "." +
                       property_name(*mem.property);
            }
        }
        return "";
    }

    std::vector<std::string> call_argument_names(const CallExpr& call) const {
        std::vector<std::string> names;
        if (call.arguments.size() == 1 && call.arguments[0] &&
            is_named_call_argument(*call.arguments[0])) {
            const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(call.arguments[0]->expr);
            for (const auto& prop : obj.properties) {
                if (prop && prop->key) {
                    names.push_back(property_name(*prop->key));
                }
            }
        }
        return names;
    }

    std::optional<Type> named_argument_type(const CallExpr& call,
                                            std::string_view name,
                                            const std::vector<Type>& arg_types) const {
        if (arg_types.empty() || call.arguments.size() != 1 || !call.arguments[0] ||
            !is_named_call_argument(*call.arguments[0]) || arg_types[0].kind != TypeKind::Record) {
            return std::nullopt;
        }
        return arg_types[0].Field(name);
    }

    Type infer_call_return_type(const CallExpr& call,
                                const ResolvedCallee& resolved,
                                bool pipe_value_present,
                                const Type& pipe_type,
                                const std::vector<Type>& arg_types) {
        if (resolved.sig == nullptr) {
            auto type = call.callee ? visit_free_callee_type(*call.callee) : Type::Dynamic();
            if (type.kind == TypeKind::Function && !type.args.empty()) {
                return type.args[0];
            }
            return Type::Dynamic();
        }
        auto base = ParseTypeExpression(resolved.sig->return_type);
        if (!IsCallableBuiltin(*resolved.sig)) {
            diagnostic("value `" + resolved.sig->fq_name + "` is not callable",
                       call.callee ? call.callee->loc : SourceLocation{},
                       DiagnosticSeverity::Error);
            return base;
        }
        const auto name = resolved.sig->name;
        if (resolved.sig->provider) {
            if (auto provider = infer_provider_return_type(call, *resolved.sig);
                provider.has_value()) {
                add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, *provider);
                return *provider;
            }
        }
        if (name == "from" && resolved.package == "array") {
            auto rows = named_object_field_type(call, "rows");
            if (rows && rows->kind == TypeKind::Array && !rows->args.empty()) {
                auto stream = Type::Stream(rows->args[0]);
                add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, stream);
                return stream;
            }
        }
        if (pipe_value_present &&
            (base.kind == TypeKind::Stream || resolved.sig->return_type == "stream[A]")) {
            if (name == "keep") {
                return apply_keep(call, pipe_type);
            }
            if (name == "drop") {
                return apply_drop(call, pipe_type);
            }
            if (name == "rename") {
                return apply_rename(call, pipe_type);
            }
            if (name == "map") {
                return apply_map(call, pipe_type, arg_types);
            }
            if (name == "group") {
                return apply_group(call, pipe_type);
            }
            if (name == "duplicate") {
                return apply_duplicate(call, pipe_type);
            }
            if (name == "set") {
                return apply_set(call, pipe_type);
            }
            add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, pipe_type);
            return pipe_type.IsUnknownLike() ? base : pipe_type;
        }
        if (name == "join" && resolved.package.empty()) {
            return apply_join(call, arg_types);
        }
        return base;
    }

    std::optional<Type> infer_provider_return_type(const CallExpr& call,
                                                   const BuiltinSignature& sig) {
        if (sig.package == "array" && sig.name == "from") {
            auto rows = named_object_field_type(call, "rows");
            if (rows && rows->kind == TypeKind::Array && !rows->args.empty()) {
                return Type::Stream(rows->args[0]);
            }
            return Type::Stream(Type::Record({}, true));
        }
        if ((sig.package == "mysql" || sig.package == "sqlite") && sig.name == "from") {
            return Type::Stream(Type::Record({}, true));
        }
        if (sig.package == "csv" && sig.name == "from") {
            auto schema = source_schema_resolver_.ResolveCsv(literal_string_argument(call, "csv"),
                                                             literal_string_argument(call, "file"),
                                                             literal_string_argument(call, "mode"));
            if (schema.has_value()) {
                return schema;
            }
            return Type::Stream(Type::Record({}, true));
        }
        return std::nullopt;
    }

    RecordFieldType field(std::string name, Type type) const {
        return make_field(std::move(name), std::move(type));
    }

    Type visit_free_callee_type(const Expression& callee) {
        if (callee.type == Expression::Type::Identifier) {
            const auto& id = *std::get<std::unique_ptr<Identifier>>(callee.expr);
            return type_for_name(id.name);
        }
        return Type::Dynamic();
    }

    std::optional<Type> named_object_field_type(const CallExpr& call, std::string_view name) {
        if (call.arguments.size() != 1 || !call.arguments[0] ||
            !is_named_call_argument(*call.arguments[0])) {
            return std::nullopt;
        }
        const auto type = expression_type_without_refs(*call.arguments[0]);
        return type.Field(name);
    }

    Type expression_type_without_refs(const Expression& expr) const {
        switch (expr.type) {
            case Expression::Type::ObjectExpr: {
                const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(expr.expr);
                std::vector<RecordFieldType> fields;
                for (const auto& prop : obj.properties) {
                    if (prop && prop->key && prop->value) {
                        fields.push_back({.name = property_name(*prop->key),
                                          .type = std::make_shared<Type>(
                                              expression_type_without_refs(*prop->value))});
                    }
                }
                return Type::Record(std::move(fields), obj.with != nullptr);
            }
            case Expression::Type::ArrayExpr: {
                const auto& arr = *std::get<std::unique_ptr<ArrayExpr>>(expr.expr);
                auto element = Type::Unknown();
                for (const auto& item : arr.elements) {
                    if (item && item->expression) {
                        element =
                            CommonType(element, expression_type_without_refs(*item->expression));
                    }
                }
                return Type::Array(element);
            }
            case Expression::Type::StringLit:
            case Expression::Type::StringExpr:
                return Type::Scalar(TypeKind::String);
            case Expression::Type::IntegerLit:
                return Type::Scalar(TypeKind::Int);
            case Expression::Type::FloatLit:
                return Type::Scalar(TypeKind::Float);
            case Expression::Type::BooleanLit:
                return Type::Scalar(TypeKind::Bool);
            case Expression::Type::DateTimeLit:
                return Type::Scalar(TypeKind::Time);
            case Expression::Type::DurationLit:
                return Type::Scalar(TypeKind::Duration);
            case Expression::Type::RegexpLit:
                return Type::Scalar(TypeKind::Regexp);
            case Expression::Type::FunctionExpr: {
                const auto& fn = *std::get<std::unique_ptr<FunctionExpr>>(expr.expr);
                if (fn.body && fn.body->type == FunctionBody::Type::Expression) {
                    auto ret = expression_type_without_refs(
                        *std::get<std::unique_ptr<Expression>>(fn.body->body));
                    return Type::Function({}, ret);
                }
                return Type::Function({}, Type::Dynamic());
            }
            case Expression::Type::ParenExpr: {
                const auto& paren = *std::get<std::unique_ptr<ParenExpr>>(expr.expr);
                return paren.expression ? expression_type_without_refs(*paren.expression)
                                        : Type::Unknown();
            }
            default:
                return Type::Dynamic();
        }
    }

    Type apply_keep(const CallExpr& call, const Type& pipe_type) {
        auto row = stream_row(pipe_type);
        auto columns = named_string_array(call, "columns");
        if (!row || columns.empty()) {
            return pipe_type;
        }
        std::vector<RecordFieldType> fields;
        for (const auto& column : columns) {
            if (auto field = row->Field(column); field.has_value()) {
                fields.push_back({.name = column, .type = std::make_shared<Type>(*field)});
            } else if (row->open_record) {
                fields.push_back({.name = column, .type = std::make_shared<Type>(Type::Dynamic())});
            }
        }
        auto result = Type::Stream(Type::Record(std::move(fields), false));
        add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, result);
        return result;
    }

    Type apply_drop(const CallExpr& call, const Type& pipe_type) {
        auto row = stream_row(pipe_type);
        auto columns = named_string_array(call, "columns");
        if (!row || columns.empty()) {
            return pipe_type;
        }
        std::vector<RecordFieldType> fields;
        for (const auto& field : row->fields) {
            if (std::find(columns.begin(), columns.end(), field.name) == columns.end()) {
                fields.push_back(field);
            }
        }
        auto result = Type::Stream(Type::Record(std::move(fields), row->open_record));
        add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, result);
        return result;
    }

    Type apply_rename(const CallExpr& call, const Type& pipe_type) {
        auto row = stream_row(pipe_type);
        auto mappings = named_string_record(call, "columns");
        if (!row || mappings.empty()) {
            return pipe_type;
        }
        std::vector<RecordFieldType> fields = row->fields;
        for (auto& field : fields) {
            const auto it = mappings.find(field.name);
            if (it != mappings.end()) {
                field.name = it->second;
            }
        }
        auto result = Type::Stream(Type::Record(std::move(fields), row->open_record));
        add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, result);
        return result;
    }

    Type apply_map(const CallExpr& call,
                   const Type& pipe_type,
                   const std::vector<Type>& arg_types) {
        auto fn = named_argument_type(call, "fn", arg_types);
        if (fn && fn->kind == TypeKind::Function && !fn->args.empty() &&
            fn->args[0].kind == TypeKind::Record) {
            auto result = Type::Stream(fn->args[0]);
            add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, result);
            return result;
        }
        return pipe_type;
    }

    Type apply_group(const CallExpr& call, const Type& pipe_type) {
        auto columns = named_string_array(call, "columns");
        const auto mode = literal_string_argument(call, "mode").value_or("by");
        if (mode == "except") {
            const std::unordered_set<std::string> excluded(columns.begin(), columns.end());
            columns.clear();
            if (const auto row = stream_row(pipe_type); row.has_value()) {
                for (const auto& field : row->fields) {
                    if (excluded.count(field.name) == 0) {
                        columns.push_back(field.name);
                    }
                }
            }
        }
        add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, pipe_type, columns);
        return pipe_type;
    }

    Type apply_duplicate(const CallExpr& call, const Type& pipe_type) {
        auto row = stream_row(pipe_type);
        const auto column = literal_string_argument(call, "column");
        const auto as = literal_string_argument(call, "as");
        if (!row || !column.has_value() || !as.has_value()) {
            return pipe_type;
        }
        auto source = row->Field(*column).value_or(Type::Dynamic());
        auto fields = row->fields;
        upsert_field(fields, *as, std::move(source));
        auto result = Type::Stream(Type::Record(std::move(fields), row->open_record));
        add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, result);
        return result;
    }

    Type apply_set(const CallExpr& call, const Type& pipe_type) {
        auto row = stream_row(pipe_type);
        const auto key = literal_string_argument(call, "key");
        if (!row || !key.has_value()) {
            return pipe_type;
        }
        auto fields = row->fields;
        upsert_field(fields, *key, Type::Scalar(TypeKind::String));
        auto result = Type::Stream(Type::Record(std::move(fields), row->open_record));
        add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, result);
        return result;
    }

    Type apply_join(const CallExpr& call, const std::vector<Type>& arg_types) {
        auto tables = named_argument_type(call, "tables", arg_types);
        const auto keys = named_string_array(call, "on");
        if (!tables || tables->kind != TypeKind::Record || tables->fields.empty()) {
            return Type::Stream(Type::Record({}, true));
        }
        if (tables->fields.size() != 2) {
            diagnostic("join currently expects exactly two input tables",
                       call.callee ? call.callee->loc : SourceLocation{},
                       DiagnosticSeverity::Error);
            return Type::Stream(Type::Record({}, true));
        }

        const auto left_row = table_row_for_join(call, tables->fields[0]);
        const auto right_row = table_row_for_join(call, tables->fields[1]);
        if (!left_row.has_value() || !right_row.has_value()) {
            return Type::Stream(Type::Record({}, true));
        }

        std::vector<RecordFieldType> fields;
        std::unordered_set<std::string> emitted;
        std::unordered_map<std::string, Type> key_types;
        const std::unordered_set<std::string> key_set(keys.begin(), keys.end());
        const auto overlapping = overlapping_join_columns(*left_row, *right_row, key_set);
        const bool open = left_row->open_record || right_row->open_record;

        auto append_table = [&](const RecordFieldType& table_field, const Type& row) {
            for (const auto& key : keys) {
                auto key_type = row.Field(key);
                if (!key_type.has_value()) {
                    if (!row.open_record) {
                        diagnostic("join key `" + key + "` is missing from table `" +
                                       table_field.name + "`",
                                   call.callee ? call.callee->loc : SourceLocation{},
                                   DiagnosticSeverity::Error);
                    }
                    continue;
                }
                const auto existing = key_types.find(key);
                if (existing != key_types.end() && !CanAssign(existing->second, *key_type)) {
                    diagnostic("join key `" + key + "` has incompatible types: " +
                                   existing->second.ToString() + " and " + key_type->ToString(),
                               call.callee ? call.callee->loc : SourceLocation{},
                               DiagnosticSeverity::Warning);
                } else {
                    key_types.emplace(key, *key_type);
                }
            }
            for (const auto& source_field : row.fields) {
                if (!source_field.type) {
                    continue;
                }
                std::string output_name = source_field.name;
                const bool is_key = key_set.count(source_field.name) != 0;
                if (!is_key && overlapping.count(output_name) != 0) {
                    output_name += "_" + table_field.name;
                }
                if (is_key && emitted.contains(output_name)) {
                    continue;
                }
                emitted.insert(output_name);
                fields.push_back({.name = std::move(output_name), .type = source_field.type});
            }
        };

        append_table(tables->fields[0], *left_row);
        append_table(tables->fields[1], *right_row);

        auto result = Type::Stream(Type::Record(std::move(fields), open));
        add_table_schema(call.callee ? call.callee->loc : SourceLocation{}, result);
        return result;
    }

    std::optional<Type> table_row_for_join(const CallExpr& call,
                                           const RecordFieldType& table_field) {
        if (!table_field.type) {
            return std::nullopt;
        }
        const auto row = stream_row(*table_field.type);
        if (!row || row->kind != TypeKind::Record) {
            diagnostic("join table `" + table_field.name + "` must be a stream, got " +
                           table_field.type->ToString(),
                       call.callee ? call.callee->loc : SourceLocation{},
                       DiagnosticSeverity::Error);
            return std::nullopt;
        }
        return row;
    }

    std::unordered_set<std::string> overlapping_join_columns(
        const Type& left_row,
        const Type& right_row,
        const std::unordered_set<std::string>& key_set) const {
        std::unordered_set<std::string> right_fields;
        for (const auto& field : right_row.fields) {
            right_fields.insert(field.name);
        }
        std::unordered_set<std::string> overlap;
        for (const auto& field : left_row.fields) {
            if (key_set.count(field.name) != 0 || right_fields.count(field.name) == 0) {
                continue;
            }
            overlap.insert(field.name);
        }
        return overlap;
    }

    std::optional<Type> stream_row(const Type& type) const {
        if ((type.kind == TypeKind::Stream || type.kind == TypeKind::Table) && !type.args.empty()) {
            return type.args[0];
        }
        return std::nullopt;
    }

    void add_table_schema(const SourceLocation& loc,
                          const Type& type,
                          std::vector<std::string> group_key = {}) {
        auto row = stream_row(type);
        if (!row || row->kind != TypeKind::Record) {
            return;
        }
        result_.table_schemas.push_back({
            .location = loc,
            .columns = row->fields,
            .group_key = std::move(group_key),
            .open = row->open_record,
        });
    }

    std::vector<std::string> named_string_array(const CallExpr& call, std::string_view name) const {
        const auto* expr = named_argument_expression(call, name);
        if (expr == nullptr || expr->type != Expression::Type::ArrayExpr) {
            return {};
        }
        std::vector<std::string> values;
        const auto& arr = *std::get<std::unique_ptr<ArrayExpr>>(expr->expr);
        for (const auto& item : arr.elements) {
            if (!item || !item->expression ||
                item->expression->type != Expression::Type::StringLit) {
                continue;
            }
            values.push_back(std::get<std::unique_ptr<StringLit>>(item->expression->expr)->value);
        }
        return values;
    }

    std::unordered_map<std::string, std::string> named_string_record(const CallExpr& call,
                                                                     std::string_view name) const {
        const auto* expr = named_argument_expression(call, name);
        if (expr == nullptr || expr->type != Expression::Type::ObjectExpr) {
            return {};
        }
        std::unordered_map<std::string, std::string> result;
        const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(expr->expr);
        for (const auto& prop : obj.properties) {
            if (!prop || !prop->key || !prop->value ||
                prop->value->type != Expression::Type::StringLit) {
                continue;
            }
            result[property_name(*prop->key)] =
                std::get<std::unique_ptr<StringLit>>(prop->value->expr)->value;
        }
        return result;
    }

    const Expression* named_argument_expression(const CallExpr& call, std::string_view name) const {
        if (call.arguments.size() != 1 || !call.arguments[0] ||
            !is_named_call_argument(*call.arguments[0])) {
            return nullptr;
        }
        const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(call.arguments[0]->expr);
        for (const auto& prop : obj.properties) {
            if (prop && prop->key && property_name(*prop->key) == name) {
                return prop->value.get();
            }
        }
        return nullptr;
    }

    std::optional<std::string> literal_string_argument(const CallExpr& call,
                                                       std::string_view name) const {
        const auto* expr = named_argument_expression(call, name);
        if (expr == nullptr || expr->type != Expression::Type::StringLit) {
            return std::nullopt;
        }
        return std::get<std::unique_ptr<StringLit>>(expr->expr)->value;
    }

    std::string unknown_argument_message(const BuiltinSignature& sig, std::string_view name) const {
        std::vector<std::string> candidates;
        candidates.reserve(sig.params.size());
        for (const auto& param : sig.params) {
            candidates.push_back(param.name);
        }
        std::string message = "unknown argument `" + std::string(name) + "` for " + sig.fq_name;
        if (auto suggestion = closest_name(name, candidates); suggestion.has_value()) {
            message += "; did you mean `" + *suggestion + "`?";
        }
        return message;
    }

    std::string unknown_package_member_message(std::string_view package,
                                               std::string_view member) const {
        std::vector<std::string> candidates;
        for (const auto* sig : BuiltinsForPackage(package)) {
            candidates.push_back(sig->name);
        }
        std::string message = "unknown function `" + std::string(member) + "` in package `" +
                              std::string(package) + "`";
        if (auto suggestion = closest_name(member, candidates); suggestion.has_value()) {
            message += "; did you mean `" + *suggestion + "`?";
        }
        return message;
    }

    void check_builtin_argument_types(const CallExpr& call,
                                      const BuiltinSignature& sig,
                                      const std::vector<Type>& arg_types,
                                      bool pipe_value_present,
                                      const Type& pipe_type) {
        if (call.arguments.size() != 1 || arg_types.empty() || !call.arguments[0] ||
            !is_named_call_argument(*call.arguments[0]) || arg_types[0].kind != TypeKind::Record) {
            return;
        }
        for (const auto& param : sig.params) {
            auto actual = param.kind == BuiltinParamKind::Pipe && pipe_value_present
                              ? std::optional<Type>(pipe_type)
                              : named_argument_type(call, param.name, arg_types);
            if (!actual.has_value()) {
                continue;
            }
            auto expected = ParseTypeExpression(param.type);
            if (!CanAssign(expected, *actual)) {
                diagnostic("argument `" + param.name + "` for " + sig.fq_name + " expects " +
                               expected.ToString() + ", got " + actual->ToString(),
                           call.callee ? call.callee->loc : SourceLocation{},
                           DiagnosticSeverity::Warning);
            }
        }
    }

    void check_builtin_call(const CallExpr& call,
                            const BuiltinSignature& sig,
                            const SourceLocation& loc,
                            bool pipe_value_present) {
        std::unordered_set<std::string> allowed;
        std::unordered_set<std::string> required;
        for (const auto& param : sig.params) {
            allowed.insert(param.name);
            if (param.kind == BuiltinParamKind::Required ||
                (param.kind == BuiltinParamKind::Pipe && !pipe_value_present)) {
                required.insert(param.name);
            }
        }

        if (call.arguments.size() == 1 && is_named_call_argument(*call.arguments[0])) {
            const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(call.arguments[0]->expr);
            std::unordered_set<std::string> seen;
            for (const auto& prop : obj.properties) {
                if (!prop || !prop->key) {
                    continue;
                }
                const auto name = property_name(*prop->key);
                if (name.empty()) {
                    continue;
                }
                if (!seen.insert(name).second) {
                    diagnostic("duplicate argument `" + name + "` for " + sig.fq_name,
                               prop->loc,
                               DiagnosticSeverity::Error);
                }
                if (!allowed.contains(name)) {
                    diagnostic(
                        unknown_argument_message(sig, name), prop->loc, DiagnosticSeverity::Error);
                }
                required.erase(name);
            }
        } else if (!call.arguments.empty()) {
            size_t positional = call.arguments.size();
            size_t max_positional = 0;
            for (const auto& param : sig.params) {
                if (param.kind != BuiltinParamKind::Pipe) {
                    ++max_positional;
                }
            }
            if (positional > max_positional) {
                diagnostic("too many positional arguments for " + sig.fq_name,
                           call.arguments.back()->loc,
                           DiagnosticSeverity::Error);
            }
            size_t index = 0;
            for (const auto& param : sig.params) {
                if (param.kind == BuiltinParamKind::Pipe) {
                    continue;
                }
                if (index++ < positional) {
                    required.erase(param.name);
                }
            }
        }

        for (const auto& name : required) {
            diagnostic("missing required argument `" + name + "` for " + sig.fq_name,
                       call.callee ? call.callee->loc : loc,
                       DiagnosticSeverity::Error);
        }
    }

    ResolvedCallee resolve_callee(const CallExpr& call) {
        if (!call.callee) {
            return {};
        }
        if (call.callee->type == Expression::Type::Identifier) {
            const auto& id = *std::get<std::unique_ptr<Identifier>>(call.callee->expr);
            return {.sig = FindUniverseBuiltinSignature(id.name),
                    .location = call.callee->loc,
                    .package_definition_id = 0,
                    .package = "",
                    .member = id.name};
        }
        if (call.callee->type != Expression::Type::MemberExpr) {
            return {};
        }
        const auto& mem = *std::get<std::unique_ptr<MemberExpr>>(call.callee->expr);
        if (!mem.object || mem.object->type != Expression::Type::Identifier || !mem.property) {
            return {};
        }
        const auto& obj = *std::get<std::unique_ptr<Identifier>>(mem.object->expr);
        const auto prop = property_name(*mem.property);
        const auto import_it = imports_.find(obj.name);
        if (import_it == imports_.end()) {
            return {};
        }
        const auto* sig = FindBuiltinSignature(import_it->second.path, prop);
        if (sig == nullptr && IsKnownPackage(import_it->second.path)) {
            diagnostic(unknown_package_member_message(import_it->second.path, prop),
                       call.callee->loc,
                       DiagnosticSeverity::Error);
        }
        return {.sig = sig,
                .location = call.callee->loc,
                .package_definition_id = import_it->second.definition_id,
                .package = import_it->second.path,
                .member = prop};
    }

    std::vector<std::string> function_param_names(const FunctionExpr& fn) {
        std::vector<std::string> params;
        for (const auto& param : fn.params) {
            if (param && param->key) {
                params.push_back(property_name(*param->key));
            }
        }
        return params;
    }

    size_t define(const std::string& name,
                  SymbolKind kind,
                  const SourceLocation& loc,
                  std::vector<std::string> params = {},
                  std::optional<std::string> import_path = std::nullopt,
                  std::optional<std::string> builtin_package = std::nullopt,
                  Type type = Type::Unknown()) {
        auto& scope = scopes_[current_scope_id_];
        const auto duplicate = scope.definitions.find(name);
        if (duplicate != scope.definitions.end()) {
            diagnostic("duplicate definition: " + name, loc, DiagnosticSeverity::Warning);
        }
        const size_t id = next_symbol_id_++;
        result_.definitions.push_back({
            .id = id,
            .name = name,
            .kind = kind,
            .location = loc,
            .scope_id = current_scope_id_,
            .import_path = std::move(import_path),
            .builtin_package = std::move(builtin_package),
            .parameters = std::move(params),
            .type = std::move(type),
        });
        scope.definitions[name] = id;
        result_.scopes[current_scope_id_].definitions.push_back(id);
        return id;
    }

    void predeclare(const std::string& name,
                    SymbolKind kind,
                    const SourceLocation& loc,
                    Type type = Type::Unknown()) {
        const auto id = define(name, kind, loc, {}, std::nullopt, std::nullopt, std::move(type));
        predeclared_definition_ids_.insert(id);
    }

    bool update_predeclared(const std::string& name,
                            SymbolKind kind,
                            std::vector<std::string> params,
                            Type type) {
        const auto& scope = scopes_[current_scope_id_];
        const auto it = scope.definitions.find(name);
        if (it == scope.definitions.end() || !predeclared_definition_ids_.contains(it->second)) {
            return false;
        }
        for (auto& def : result_.definitions) {
            if (def.id == it->second) {
                def.kind = kind;
                def.parameters = std::move(params);
                def.type = std::move(type);
                predeclared_definition_ids_.erase(it->second);
                return true;
            }
        }
        return false;
    }

    size_t reference(const std::string& name, const SourceLocation& loc, ReferenceKind kind) {
        auto definition_id = resolve(name);
        const bool resolved = definition_id != 0 || FindUniverseBuiltinSignature(name) != nullptr ||
                              is_intrinsic(name);
        const auto index = result_.references.size();
        result_.references.push_back({
            .name = name,
            .location = loc,
            .kind = kind,
            .scope_id = current_scope_id_,
            .definition_id = definition_id,
            .package = std::nullopt,
            .member = std::nullopt,
            .resolved = resolved,
        });
        if (!resolved) {
            std::string message = "undefined identifier: " + name;
            if (auto suggestion = closest_name(name, accessible_symbol_names());
                suggestion.has_value()) {
                message += "; did you mean `" + *suggestion + "`?";
            }
            diagnostic(std::move(message), loc, DiagnosticSeverity::Warning);
        }
        return index;
    }

    std::vector<std::string> accessible_symbol_names() const {
        std::vector<std::string> names;
        std::unordered_set<std::string> seen;
        size_t scope_index = current_scope_id_;
        while (true) {
            const auto& scope = scopes_[scope_index];
            for (const auto& [name, id] : scope.definitions) {
                (void)id;
                if (seen.insert(name).second) {
                    names.push_back(name);
                }
            }
            if (scope.id == scope.parent_id) {
                break;
            }
            scope_index = scope.parent_id;
        }
        for (const auto& sig : AllBuiltinSignatures()) {
            if (sig.package.empty() && seen.insert(sig.name).second) {
                names.push_back(sig.name);
            }
        }
        return names;
    }

    size_t resolve(const std::string& name) const {
        size_t scope_index = current_scope_id_;
        while (true) {
            const auto& scope = scopes_[scope_index];
            const auto it = scope.definitions.find(name);
            if (it != scope.definitions.end()) {
                return it->second;
            }
            if (scope.id == scope.parent_id) {
                return 0;
            }
            scope_index = scope.parent_id;
        }
    }

    bool is_intrinsic(const std::string& name) const {
        static const std::unordered_set<std::string> names = {
            "true",
            "false",
            "nothing",
            "exists",
            "int",
            "uint",
            "float",
            "string",
            "bool",
            "time",
            "duration",
            "bytes",
            "regexp",
            "dynamic",
        };
        return names.contains(name);
    }

    void push_scope(const SourceLocation& loc) {
        const auto parent = scopes_.empty() ? 0 : current_scope_id_;
        Scope scope{.id = scopes_.size(), .parent_id = scopes_.empty() ? 0 : parent};
        scopes_.push_back(std::move(scope));
        current_scope_id_ = scopes_.back().id;
        result_.scopes.push_back({
            .id = current_scope_id_,
            .parent_id = scopes_[current_scope_id_].parent_id,
            .location = loc,
            .definitions = {},
        });
    }

    void pop_scope() {
        if (scopes_.empty()) {
            return;
        }
        current_scope_id_ = scopes_[current_scope_id_].parent_id;
    }

    void diagnostic(std::string message, const SourceLocation& loc, DiagnosticSeverity severity) {
        result_.diagnostics.push_back({
            .message = std::move(message),
            .location = loc,
            .severity = severity,
        });
    }

    struct ImportBinding {
        std::string path;
        size_t definition_id = 0;
    };

    AnalysisResult result_;
    std::vector<Scope> scopes_;
    std::unordered_set<size_t> predeclared_definition_ids_;
    std::unordered_map<std::string, ImportBinding> imports_;
    SourceSchemaResolver source_schema_resolver_;
    size_t current_scope_id_ = 0;
    size_t next_symbol_id_ = 1;
};

} // namespace

const Symbol* AnalysisResult::FindDefinition(size_t id) const {
    for (const auto& def : definitions) {
        if (def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

const Symbol* AnalysisResult::FindDefinition(std::string_view name) const {
    for (const auto& def : definitions) {
        if (def.name == name) {
            return &def;
        }
    }
    return nullptr;
}

const Symbol* AnalysisResult::DefinitionAt(uint32_t line, uint32_t column) const {
    const Symbol* best = nullptr;
    uint64_t best_span = std::numeric_limits<uint64_t>::max();
    for (const auto& def : definitions) {
        if (!position_in_range(line, column, def.location)) {
            continue;
        }
        const auto span = location_span(def.location);
        if (span < best_span) {
            best = &def;
            best_span = span;
        }
    }
    return best;
}

const SymbolReference* AnalysisResult::ReferenceAt(uint32_t line, uint32_t column) const {
    const SymbolReference* best = nullptr;
    uint64_t best_span = std::numeric_limits<uint64_t>::max();
    for (const auto& ref : references) {
        if (!position_in_range(line, column, ref.location)) {
            continue;
        }
        const auto span = location_span(ref.location);
        if (span < best_span) {
            best = &ref;
            best_span = span;
        }
    }
    return best;
}

const Symbol* AnalysisResult::DefinitionForSymbolAt(uint32_t line, uint32_t column) const {
    if (const auto* ref = ReferenceAt(line, column); ref != nullptr) {
        return FindDefinition(ref->definition_id);
    }
    return DefinitionAt(line, column);
}

const ExpressionInfo* AnalysisResult::ExpressionAt(uint32_t line, uint32_t column) const {
    const ExpressionInfo* best = nullptr;
    uint64_t best_span = std::numeric_limits<uint64_t>::max();
    for (const auto& expr : expressions) {
        if (!position_in_range(line, column, expr.location)) {
            continue;
        }
        const auto span = location_span(expr.location);
        if (span < best_span) {
            best = &expr;
            best_span = span;
        }
    }
    return best;
}

std::optional<Type> AnalysisResult::TypeAt(uint32_t line, uint32_t column) const {
    if (const auto* expr = ExpressionAt(line, column); expr != nullptr) {
        return expr->type;
    }
    if (const auto* def = DefinitionAt(line, column); def != nullptr) {
        return def->type;
    }
    return std::nullopt;
}

const CallInfo* AnalysisResult::CallAt(uint32_t line, uint32_t column) const {
    const CallInfo* best = nullptr;
    uint64_t best_span = std::numeric_limits<uint64_t>::max();
    for (const auto& call : calls) {
        if (!position_in_range(line, column, call.callee_location)) {
            continue;
        }
        const auto span = location_span(call.callee_location);
        if (span < best_span) {
            best = &call;
            best_span = span;
        }
    }
    return best;
}

const TableSchema* AnalysisResult::TableSchemaAt(uint32_t line, uint32_t column) const {
    const TableSchema* best = nullptr;
    uint64_t best_span = std::numeric_limits<uint64_t>::max();
    for (const auto& schema : table_schemas) {
        if (!position_in_range(line, column, schema.location)) {
            continue;
        }
        const auto span = location_span(schema.location);
        if (span < best_span) {
            best = &schema;
            best_span = span;
        }
    }
    return best;
}

std::string AnalysisResult::SymbolAt(uint32_t line, uint32_t column) const {
    if (const auto* def = DefinitionAt(line, column); def != nullptr) {
        return def->name;
    }
    if (const auto* ref = ReferenceAt(line, column); ref != nullptr) {
        return ref->name;
    }
    return "";
}

std::vector<const SymbolReference*> AnalysisResult::ReferencesOf(const Symbol& def) const {
    std::vector<const SymbolReference*> result;
    for (const auto& ref : references) {
        if (ref.definition_id == def.id) {
            result.push_back(&ref);
        }
    }
    return result;
}

std::vector<const SymbolReference*> AnalysisResult::ReferencesOf(std::string_view name) const {
    std::vector<const SymbolReference*> result;
    for (const auto& ref : references) {
        if (ref.name == name) {
            result.push_back(&ref);
        }
    }
    return result;
}

AnalysisResult SemanticAnalyzer::Analyze(const File& file) {
    return Binder().Bind(file);
}

const Symbol* PackageAnalysisResult::FindExport(std::string_view name) const {
    for (const auto& symbol : exports) {
        if (symbol.name == name) {
            return &symbol;
        }
    }
    return nullptr;
}

PackageAnalysisResult PackageAnalyzer::Analyze(const Package& package) {
    PackageAnalysisResult result;
    SemanticAnalyzer analyzer;
    std::unordered_map<std::string, SourceLocation> exported_names;
    for (const auto& file : package.files) {
        if (!file) {
            continue;
        }
        result.files.push_back(analyzer.Analyze(*file));
        for (const auto& def : result.files.back().definitions) {
            if (def.scope_id != 0 || def.kind == SymbolKind::Import ||
                def.kind == SymbolKind::Parameter) {
                continue;
            }
            const auto [it, inserted] = exported_names.emplace(def.name, def.location);
            if (!inserted) {
                result.diagnostics.push_back({
                    .message = "duplicate package export: " + def.name,
                    .location = def.location,
                    .severity = DiagnosticSeverity::Error,
                });
            }
            result.exports.push_back(def);
        }
        result.diagnostics.insert(result.diagnostics.end(),
                                  result.files.back().diagnostics.begin(),
                                  result.files.back().diagnostics.end());
    }
    return result;
}

} // namespace pl::flux::analysis
