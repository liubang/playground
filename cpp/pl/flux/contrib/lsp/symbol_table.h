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

#pragma once

#include "cpp/pl/flux/syntax/ast.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pl::flux::lsp {

// A location reference in source code.
struct Location {
    std::string uri;
    uint32_t start_line = 0; // 1-based (from AST)
    uint32_t start_col = 0;
    uint32_t end_line = 0;
    uint32_t end_col = 0;
};

// Symbol kinds recognized by the analyzer.
enum class SymbolKind {
    Variable,
    Function,
    Option,
    Builtin,
    Import,
    Parameter,
};

// A definition of a symbol.
struct SymbolDef {
    size_t id = 0;
    std::string name;
    SymbolKind kind;
    Location location;
    // For functions: parameter names for signatureHelp
    std::vector<std::string> parameters;
};

// A reference (usage) of a symbol.
struct SymbolRef {
    std::string name;
    Location location;
    size_t definition_id = 0;
};

// The result of analyzing a single file: definitions and references.
struct SymbolTable {
    std::vector<SymbolDef> definitions;
    std::vector<SymbolRef> references;
    // Import paths discovered
    std::vector<std::string> imported_packages;

    // Find the definition for a symbol at the given 1-based position.
    // Returns nullptr if no definition matches.
    const SymbolDef* definition_at(uint32_t line, uint32_t col) const;

    // Find all references of a given symbol name.
    std::vector<const SymbolRef*> references_of(const std::string& name) const;

    // Find all references resolved to a concrete definition.
    std::vector<const SymbolRef*> references_of(const SymbolDef& def) const;

    // Find the definition of a given symbol name.
    const SymbolDef* find_definition(const std::string& name) const;

    // Find the concrete definition for the symbol under a given position.
    const SymbolDef* definition_for_symbol_at(uint32_t line, uint32_t col) const;

    // Find the concrete definition for a known symbol name near a given position.
    const SymbolDef* definition_for_symbol_at(const std::string& name,
                                              uint32_t line,
                                              uint32_t col) const;

    // Find the symbol (def or ref) at a given position.
    // Returns the symbol name, or empty string if nothing found.
    std::string symbol_at(uint32_t line, uint32_t col) const;

    // Diagnostics: undefined references (references without matching definitions)
    struct Diagnostic {
        std::string message;
        Location location;
        int severity; // 1=error, 2=warning, 3=info, 4=hint
    };
    std::vector<Diagnostic> diagnostics;
};

// Build a symbol table from a parsed File AST.
// The uri is used to tag all locations.
SymbolTable build_symbol_table(const File& file, const std::string& uri);

} // namespace pl::flux::lsp
