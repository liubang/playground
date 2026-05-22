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
// Created: 2026/05/18 19:19

#include "cpp/pl/flux/contrib/lsp/symbol_table.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace pl::flux::lsp {

namespace {

// Check if a position (1-based) falls within a location range.
bool position_in_range(uint32_t line, uint32_t col, const Location& loc) {
    if (line < loc.start_line || line > loc.end_line) {
        return false;
    }
    if (line == loc.start_line && col < loc.start_col) {
        return false;
    }
    if (line == loc.end_line && col > loc.end_col) {
        return false;
    }
    return true;
}

Location make_location(const std::string& uri, const SourceLocation& sloc) {
    return {
        .uri = uri,
        .start_line = sloc.start.line,
        .start_col = sloc.start.column,
        .end_line = sloc.end.line,
        .end_col = sloc.end.column,
    };
}

SourceLocation make_import_name_location(const ImportDeclaration& imp, const std::string& name) {
    SourceLocation loc = imp.loc;
    if (name.empty()) {
        return loc;
    }

    uint32_t start_col = imp.loc.start.column;
    if (imp.alias && !imp.alias->name.empty()) {
        // import alias "path": alias starts right after "import ".
        start_col += 7;
    } else if (imp.path) {
        // import "pkg/path": default package name is the last path segment inside the quotes.
        const auto& path = imp.path->value;
        const auto name_offset = path.size() >= name.size() ? path.size() - name.size() : 0;
        start_col += 8 + static_cast<uint32_t>(name_offset);
    }

    loc.start.line = imp.loc.start.line;
    loc.start.column = start_col;
    loc.end.line = imp.loc.start.line;
    loc.end.column = start_col + static_cast<uint32_t>(name.size());
    return loc;
}

// Forward declarations for recursive traversal
class SymbolCollector {
public:
    explicit SymbolCollector(const std::string& uri) : uri_(uri) {}

    void visit_file(const File& file);
    SymbolTable take_result();

private:
    void visit_statement(const Statement& stmt);
    void visit_expression(const Expression& expr);
    void visit_function_expr(const FunctionExpr& fn, const SourceLocation& loc);
    void visit_property(const Property& prop);
    void visit_block(const Block& block);

    void add_definition(const std::string& name,
                        SymbolKind kind,
                        const SourceLocation& loc,
                        std::vector<std::string> params = {});
    void add_reference(const std::string& name, const SourceLocation& loc);
    void push_scope();
    void pop_scope();
    size_t resolve_definition_id(const std::string& name) const;

    std::string uri_;
    SymbolTable table_;
    struct Scope {
        size_t parent = 0;
        std::unordered_map<std::string, size_t> definitions;
    };
    std::vector<Scope> scopes_{{.parent = 0, .definitions = {}}};
    size_t current_scope_ = 0;
    size_t next_definition_id_ = 1;
};

void SymbolCollector::visit_file(const File& file) {
    // Collect imports first (they define names)
    for (const auto& imp : file.imports) {
        if (!imp->path) {
            continue;
        }
        std::string pkg_name;
        if (imp->alias && !imp->alias->name.empty()) {
            pkg_name = imp->alias->name;
        } else {
            // Extract last segment of import path as the default name
            const auto& path = imp->path->value;
            auto slash = path.rfind('/');
            pkg_name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
        }
        if (!pkg_name.empty()) {
            add_definition(pkg_name, SymbolKind::Import, make_import_name_location(*imp, pkg_name));
            table_.imported_packages.push_back(imp->path->value);
        }
    }

    // Visit body statements
    for (const auto& stmt : file.body) {
        visit_statement(*stmt);
    }

    // Produce diagnostics: references to undefined names
    // (Skip common universe builtins and keywords)
    static const std::unordered_set<std::string> universe_names = {
        "from",
        "to",
        "range",
        "filter",
        "map",
        "limit",
        "tail",
        "keep",
        "drop",
        "rename",
        "sort",
        "group",
        "pivot",
        "fill",
        "union",
        "count",
        "sum",
        "mean",
        "min",
        "max",
        "spread",
        "quantile",
        "median",
        "first",
        "last",
        "top",
        "bottom",
        "reduce",
        "distinct",
        "window",
        "aggregateWindow",
        "elapsed",
        "difference",
        "derivative",
        "join",
        "columns",
        "keys",
        "findColumn",
        "findRecord",
        "explain",
        "yield",
        "true",
        "false",
        "nothing",
        "die",
        "contains",
        "length",
        "string",
        "int",
        "float",
        "uint",
        "bool",
        "time",
        "duration",
        "bytes",
        "display",
        "exists",
    };

    for (const auto& ref : table_.references) {
        if (ref.definition_id != 0) {
            continue;
        }
        if (universe_names.count(ref.name) > 0) {
            continue;
        }
        table_.diagnostics.push_back({
            .message = "undefined identifier: " + ref.name,
            .location = ref.location,
            .severity = 2, // warning (not error — could be external)
        });
    }
}

void SymbolCollector::visit_statement(const Statement& stmt) {
    switch (stmt.type) {
        case Statement::Type::VariableAssignment: {
            const auto& va = *std::get<std::unique_ptr<VariableAssgn>>(stmt.stmt);
            if (va.id && !va.id->name.empty()) {
                std::vector<std::string> params;
                if (va.init && va.init->type == Expression::Type::FunctionExpr) {
                    const auto& fn = *std::get<std::unique_ptr<FunctionExpr>>(va.init->expr);
                    for (const auto& p : fn.params) {
                        if (p->key && p->key->type == PropertyKey::Type::Identifier) {
                            const auto& id = *std::get<std::unique_ptr<Identifier>>(p->key->key);
                            params.push_back(id.name);
                        }
                    }
                    add_definition(va.id->name, SymbolKind::Function, stmt.loc, std::move(params));
                    // Visit function body (defines params as local scope)
                    visit_function_expr(fn, stmt.loc);
                } else {
                    add_definition(va.id->name, SymbolKind::Variable, stmt.loc);
                    if (va.init) {
                        visit_expression(*va.init);
                    }
                }
            }
            break;
        }
        case Statement::Type::OptionStatement: {
            const auto& opt = *std::get<std::unique_ptr<OptionStmt>>(stmt.stmt);
            if (opt.assignment && opt.assignment->type == Assignment::Type::VariableAssignment) {
                const auto& va = *std::get<std::unique_ptr<VariableAssgn>>(opt.assignment->value);
                if (va.id && !va.id->name.empty()) {
                    add_definition(va.id->name, SymbolKind::Option, stmt.loc);
                    if (va.init) {
                        visit_expression(*va.init);
                    }
                }
            }
            break;
        }
        case Statement::Type::BuiltinStatement: {
            const auto& bi = *std::get<std::unique_ptr<BuiltinStmt>>(stmt.stmt);
            if (bi.id && !bi.id->name.empty()) {
                add_definition(bi.id->name, SymbolKind::Builtin, stmt.loc);
            }
            break;
        }
        case Statement::Type::ExpressionStatement: {
            const auto& es = *std::get<std::unique_ptr<ExprStmt>>(stmt.stmt);
            if (es.expression) {
                visit_expression(*es.expression);
            }
            break;
        }
        case Statement::Type::ReturnStatement: {
            const auto& rs = *std::get<std::unique_ptr<ReturnStmt>>(stmt.stmt);
            if (rs.argument) {
                visit_expression(*rs.argument);
            }
            break;
        }
        case Statement::Type::TestCaseStatement: {
            const auto& tc = *std::get<std::unique_ptr<TestCaseStmt>>(stmt.stmt);
            if (tc.id && !tc.id->name.empty()) {
                add_definition(tc.id->name, SymbolKind::Function, stmt.loc);
            }
            if (tc.block) {
                visit_block(*tc.block);
            }
            break;
        }
        default:
            break;
    }
}

void SymbolCollector::visit_expression(const Expression& expr) {
    switch (expr.type) {
        case Expression::Type::Identifier: {
            const auto& id = *std::get<std::unique_ptr<Identifier>>(expr.expr);
            if (!id.name.empty()) {
                add_reference(id.name, expr.loc);
            }
            break;
        }
        case Expression::Type::CallExpr: {
            const auto& call = *std::get<std::unique_ptr<CallExpr>>(expr.expr);
            if (call.callee) {
                visit_expression(*call.callee);
            }
            for (const auto& arg : call.arguments) {
                if (arg) {
                    visit_expression(*arg);
                }
            }
            break;
        }
        case Expression::Type::MemberExpr: {
            const auto& mem = *std::get<std::unique_ptr<MemberExpr>>(expr.expr);
            if (mem.object) {
                visit_expression(*mem.object);
            }
            // Don't add member property as a reference (it's qualified by object)
            break;
        }
        case Expression::Type::PipeExpr: {
            const auto& pipe = *std::get<std::unique_ptr<PipeExpr>>(expr.expr);
            if (pipe.argument) {
                visit_expression(*pipe.argument);
            }
            if (pipe.call) {
                // CallExpr has callee (Expression) and arguments (vector<Expression>)
                if (pipe.call->callee) {
                    visit_expression(*pipe.call->callee);
                }
                for (const auto& arg : pipe.call->arguments) {
                    if (arg) {
                        visit_expression(*arg);
                    }
                }
            }
            break;
        }
        case Expression::Type::BinaryExpr: {
            const auto& bin = *std::get<std::unique_ptr<BinaryExpr>>(expr.expr);
            if (bin.left) {
                visit_expression(*bin.left);
            }
            if (bin.right) {
                visit_expression(*bin.right);
            }
            break;
        }
        case Expression::Type::UnaryExpr: {
            const auto& un = *std::get<std::unique_ptr<UnaryExpr>>(expr.expr);
            if (un.argument) {
                visit_expression(*un.argument);
            }
            break;
        }
        case Expression::Type::LogicalExpr: {
            const auto& log = *std::get<std::unique_ptr<LogicalExpr>>(expr.expr);
            if (log.left) {
                visit_expression(*log.left);
            }
            if (log.right) {
                visit_expression(*log.right);
            }
            break;
        }
        case Expression::Type::ConditionalExpr: {
            const auto& cond = *std::get<std::unique_ptr<ConditionalExpr>>(expr.expr);
            if (cond.test) {
                visit_expression(*cond.test);
            }
            if (cond.consequent) {
                visit_expression(*cond.consequent);
            }
            if (cond.alternate) {
                visit_expression(*cond.alternate);
            }
            break;
        }
        case Expression::Type::ObjectExpr: {
            const auto& obj = *std::get<std::unique_ptr<ObjectExpr>>(expr.expr);
            if (obj.with && obj.with->source && !obj.with->source->name.empty()) {
                add_reference(obj.with->source->name, expr.loc);
            }
            for (const auto& prop : obj.properties) {
                if (prop) {
                    visit_property(*prop);
                }
            }
            break;
        }
        case Expression::Type::ArrayExpr: {
            const auto& arr = *std::get<std::unique_ptr<ArrayExpr>>(expr.expr);
            for (const auto& elem : arr.elements) {
                if (elem && elem->expression) {
                    visit_expression(*elem->expression);
                }
            }
            break;
        }
        case Expression::Type::FunctionExpr: {
            const auto& fn = *std::get<std::unique_ptr<FunctionExpr>>(expr.expr);
            visit_function_expr(fn, expr.loc);
            break;
        }
        case Expression::Type::IndexExpr: {
            const auto& idx = *std::get<std::unique_ptr<IndexExpr>>(expr.expr);
            if (idx.array) {
                visit_expression(*idx.array);
            }
            if (idx.index) {
                visit_expression(*idx.index);
            }
            break;
        }
        case Expression::Type::ParenExpr: {
            const auto& paren = *std::get<std::unique_ptr<ParenExpr>>(expr.expr);
            if (paren.expression) {
                visit_expression(*paren.expression);
            }
            break;
        }
        default:
            // Literals and other terminal expressions — nothing to traverse
            break;
    }
}

void SymbolCollector::visit_function_expr(const FunctionExpr& fn, const SourceLocation& /*loc*/) {
    push_scope();

    // Function parameters are definitions within the function scope
    for (const auto& p : fn.params) {
        if (p->key && p->key->type == PropertyKey::Type::Identifier) {
            const auto& id = *std::get<std::unique_ptr<Identifier>>(p->key->key);
            if (!id.name.empty()) {
                add_definition(id.name, SymbolKind::Parameter, p->loc);
            }
        }
        // Default values are expressions
        if (p->value) {
            visit_expression(*p->value);
        }
    }

    // Visit body
    if (fn.body) {
        if (fn.body->type == FunctionBody::Type::Block) {
            const auto& block = *std::get<std::unique_ptr<Block>>(fn.body->body);
            visit_block(block);
        } else {
            const auto& body_expr = *std::get<std::unique_ptr<Expression>>(fn.body->body);
            visit_expression(body_expr);
        }
    }

    pop_scope();
}

void SymbolCollector::visit_property(const Property& prop) {
    // Property values are expressions (keys are not added as references)
    if (prop.value) {
        visit_expression(*prop.value);
    }
}

void SymbolCollector::visit_block(const Block& block) {
    for (const auto& stmt : block.body) {
        if (stmt) {
            visit_statement(*stmt);
        }
    }
}

void SymbolCollector::add_definition(const std::string& name,
                                     SymbolKind kind,
                                     const SourceLocation& loc,
                                     std::vector<std::string> params) {
    const size_t id = next_definition_id_++;
    table_.definitions.push_back({
        .id = id,
        .name = name,
        .kind = kind,
        .location = make_location(uri_, loc),
        .parameters = std::move(params),
    });
    scopes_[current_scope_].definitions[name] = id;
}

void SymbolCollector::add_reference(const std::string& name, const SourceLocation& loc) {
    table_.references.push_back({
        .name = name,
        .location = make_location(uri_, loc),
        .definition_id = resolve_definition_id(name),
    });
}

void SymbolCollector::push_scope() {
    scopes_.push_back({.parent = current_scope_, .definitions = {}});
    current_scope_ = scopes_.size() - 1;
}

void SymbolCollector::pop_scope() {
    if (current_scope_ != 0) {
        current_scope_ = scopes_[current_scope_].parent;
    }
}

size_t SymbolCollector::resolve_definition_id(const std::string& name) const {
    size_t scope = current_scope_;
    while (true) {
        const auto it = scopes_[scope].definitions.find(name);
        if (it != scopes_[scope].definitions.end()) {
            return it->second;
        }
        if (scope == 0) {
            break;
        }
        scope = scopes_[scope].parent;
    }
    return 0;
}

SymbolTable SymbolCollector::take_result() {
    return std::move(table_);
}

} // namespace

// ============================================================
// SymbolTable query methods
// ============================================================

const SymbolDef* SymbolTable::definition_at(uint32_t line, uint32_t col) const {
    for (const auto& def : definitions) {
        if (position_in_range(line, col, def.location)) {
            return &def;
        }
    }
    return nullptr;
}

std::vector<const SymbolRef*> SymbolTable::references_of(const std::string& name) const {
    std::vector<const SymbolRef*> result;
    for (const auto& ref : references) {
        if (ref.name == name) {
            result.push_back(&ref);
        }
    }
    return result;
}

std::vector<const SymbolRef*> SymbolTable::references_of(const SymbolDef& def) const {
    std::vector<const SymbolRef*> result;
    for (const auto& ref : references) {
        if (ref.definition_id == def.id) {
            result.push_back(&ref);
        }
    }
    return result;
}

const SymbolDef* SymbolTable::find_definition(const std::string& name) const {
    for (const auto& def : definitions) {
        if (def.name == name) {
            return &def;
        }
    }
    return nullptr;
}

const SymbolDef* SymbolTable::definition_for_symbol_at(uint32_t line, uint32_t col) const {
    for (const auto& ref : references) {
        if (position_in_range(line, col, ref.location)) {
            for (const auto& def : definitions) {
                if (def.id == ref.definition_id) {
                    return &def;
                }
            }
            return nullptr;
        }
    }

    const SymbolDef* best = nullptr;
    uint64_t best_span = std::numeric_limits<uint64_t>::max();
    for (const auto& def : definitions) {
        if (!position_in_range(line, col, def.location)) {
            continue;
        }
        const uint64_t line_span =
            static_cast<uint64_t>(def.location.end_line - def.location.start_line);
        const uint64_t col_span =
            def.location.end_col >= def.location.start_col
                ? static_cast<uint64_t>(def.location.end_col - def.location.start_col)
                : 0;
        const uint64_t span = line_span * 1000000ULL + col_span;
        if (span < best_span) {
            best = &def;
            best_span = span;
        }
    }
    return best;
}

const SymbolDef* SymbolTable::definition_for_symbol_at(const std::string& name,
                                                       uint32_t line,
                                                       uint32_t col) const {
    for (const auto& ref : references) {
        if (ref.name == name && position_in_range(line, col, ref.location)) {
            for (const auto& def : definitions) {
                if (def.id == ref.definition_id) {
                    return &def;
                }
            }
            return nullptr;
        }
    }

    for (const auto& def : definitions) {
        if (def.name == name && position_in_range(line, col, def.location)) {
            return &def;
        }
    }

    const SymbolRef* same_line_ref = nullptr;
    for (const auto& ref : references) {
        if (ref.name == name && ref.location.start_line == line) {
            if (same_line_ref != nullptr) {
                same_line_ref = nullptr;
                break;
            }
            same_line_ref = &ref;
        }
    }
    if (same_line_ref != nullptr) {
        for (const auto& def : definitions) {
            if (def.id == same_line_ref->definition_id) {
                return &def;
            }
        }
        return nullptr;
    }

    return find_definition(name);
}

std::string SymbolTable::symbol_at(uint32_t line, uint32_t col) const {
    // Check definitions first
    for (const auto& def : definitions) {
        if (position_in_range(line, col, def.location)) {
            return def.name;
        }
    }
    // Then references
    for (const auto& ref : references) {
        if (position_in_range(line, col, ref.location)) {
            return ref.name;
        }
    }
    return "";
}

// ============================================================
// Public API
// ============================================================

SymbolTable build_symbol_table(const File& file, const std::string& uri) {
    SymbolCollector collector(uri);
    collector.visit_file(file);
    return collector.take_result();
}

} // namespace pl::flux::lsp
