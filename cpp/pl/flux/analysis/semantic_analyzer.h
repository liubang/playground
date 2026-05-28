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

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "cpp/pl/flux/analysis/semantic_model.h"
#include "cpp/pl/flux/syntax/ast.h"

namespace pl::flux::analysis {

enum class DiagnosticSeverity {
    Error = 1,
    Warning = 2,
    Info = 3,
    Hint = 4,
};

enum class SymbolKind {
    Variable,
    Function,
    Option,
    Builtin,
    Import,
    Parameter,
    PackageBuiltin,
};

enum class ReferenceKind {
    Identifier,
    Callee,
    PackageObject,
    PackageMember,
    WithSource,
};

struct SemanticDiagnostic {
    std::string message;
    SourceLocation location;
    DiagnosticSeverity severity = DiagnosticSeverity::Warning;
};

struct Symbol {
    size_t id = 0;
    std::string name;
    SymbolKind kind = SymbolKind::Variable;
    SourceLocation location;
    size_t scope_id = 0;
    std::optional<std::string> import_path;
    std::optional<std::string> builtin_package;
    std::vector<std::string> parameters;
    Type type = Type::Unknown();
};

struct SymbolReference {
    std::string name;
    SourceLocation location;
    ReferenceKind kind = ReferenceKind::Identifier;
    size_t scope_id = 0;
    size_t definition_id = 0;
    std::optional<std::string> package;
    std::optional<std::string> member;
    bool resolved = false;
};

struct ScopeInfo {
    size_t id = 0;
    size_t parent_id = 0;
    SourceLocation location;
    std::vector<size_t> definitions;
};

struct AnalysisResult {
    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<Symbol> definitions;
    std::vector<SymbolReference> references;
    std::vector<ScopeInfo> scopes;
    std::vector<std::string> imported_packages;
    std::vector<ExpressionInfo> expressions;
    std::vector<CallInfo> calls;
    std::vector<RecordSchema> record_schemas;
    std::vector<TableSchema> table_schemas;

    const Symbol* FindDefinition(size_t id) const;
    const Symbol* FindDefinition(std::string_view name) const;
    const Symbol* DefinitionAt(uint32_t line, uint32_t column) const;
    const SymbolReference* ReferenceAt(uint32_t line, uint32_t column) const;
    const Symbol* DefinitionForSymbolAt(uint32_t line, uint32_t column) const;
    const ExpressionInfo* ExpressionAt(uint32_t line, uint32_t column) const;
    std::optional<Type> TypeAt(uint32_t line, uint32_t column) const;
    const CallInfo* CallAt(uint32_t line, uint32_t column) const;
    const TableSchema* TableSchemaAt(uint32_t line, uint32_t column) const;
    std::string SymbolAt(uint32_t line, uint32_t column) const;
    std::vector<const SymbolReference*> ReferencesOf(const Symbol& def) const;
    std::vector<const SymbolReference*> ReferencesOf(std::string_view name) const;
};

class SemanticAnalyzer {
public:
    struct Options {
        std::string source_base_dir;
    };

    AnalysisResult Analyze(const File& file, Options options = {});
};

struct PackageAnalysisResult {
    std::vector<AnalysisResult> files;
    std::vector<Symbol> exports;
    std::vector<SemanticDiagnostic> diagnostics;

    const Symbol* FindExport(std::string_view name) const;
};

class PackageAnalyzer {
public:
    PackageAnalysisResult Analyze(const Package& package);
};

} // namespace pl::flux::analysis
