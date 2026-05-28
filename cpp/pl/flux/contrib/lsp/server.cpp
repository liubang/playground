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
// Created: 2026/05/18 00:27

#include "cpp/pl/flux/contrib/lsp/server.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "cpp/pl/flux/analysis/builtin_metadata.h"
#include "cpp/pl/flux/analysis/semantic_analyzer.h"
#include "cpp/pl/flux/contrib/lsp/formatter.h"
#include "cpp/pl/flux/contrib/lsp/json_util.h"
#include "cpp/pl/flux/syntax/parser.h"
#include "simdjson.h"

namespace pl::flux::lsp {

namespace {

// ============================================================
// JSON helpers
// ============================================================

std::string get_string_field(const simdjson::dom::element& elem, std::string_view pointer) {
    auto val = elem.at_pointer(pointer);
    if (val.error()) {
        return "";
    }
    auto s = val.get_string();
    if (s.error()) {
        return "";
    }
    return std::string(s.value());
}

int64_t get_int_field(const simdjson::dom::element& elem, std::string_view pointer) {
    auto val = elem.at_pointer(pointer);
    if (val.error()) {
        return 0;
    }
    auto n = val.get_int64();
    if (n.error()) {
        return 0;
    }
    return n.value();
}

// ============================================================
// Text utilities
// ============================================================

bool is_ident_char(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

size_t utf8_codepoint_length(unsigned char c) {
    if ((c & 0x80) == 0) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

uint32_t utf16_units_for_utf8(std::string_view s, size_t offset) {
    uint32_t units = 0;
    for (size_t i = 0; i < offset && i < s.size();) {
        const auto len = utf8_codepoint_length(static_cast<unsigned char>(s[i]));
        units += len == 4 ? 2 : 1;
        i += std::min(len, s.size() - i);
    }
    return units;
}

// Find the byte offset of a given 0-based line in content.
size_t line_offset(const std::string& content, int line) {
    int current_line = 0;
    size_t offset = 0;
    for (size_t i = 0; i < content.size() && current_line < line; ++i) {
        if (content[i] == '\n') {
            ++current_line;
            offset = i + 1;
        }
    }
    return offset;
}

size_t next_line_offset(const std::string& content, size_t line_start) {
    const auto nl = content.find('\n', line_start);
    return nl == std::string::npos ? content.size() : nl;
}

size_t lsp_position_to_offset(const std::string& content, int line, int character) {
    const size_t ls = line_offset(content, line);
    const size_t le = next_line_offset(content, ls);
    const uint32_t target = character <= 0 ? 0 : static_cast<uint32_t>(character);
    uint32_t units = 0;
    size_t i = ls;
    while (i < le && units < target) {
        const auto len = utf8_codepoint_length(static_cast<unsigned char>(content[i]));
        const uint32_t cp_units = len == 4 ? 2 : 1;
        if (units + cp_units > target) {
            break;
        }
        units += cp_units;
        i += std::min(len, le - i);
    }
    return i;
}

uint32_t lsp_position_to_byte_column(const std::string& content, int line, int character) {
    const size_t ls = line_offset(content, line);
    const size_t pos = lsp_position_to_offset(content, line, character);
    return static_cast<uint32_t>(pos - ls + 1);
}

uint32_t byte_column_to_lsp_character(const std::string& content,
                                      uint32_t line_1based,
                                      uint32_t byte_col_1based) {
    if (line_1based == 0 || byte_col_1based == 0) {
        return 0;
    }
    const size_t ls = line_offset(content, static_cast<int>(line_1based - 1));
    const size_t le = next_line_offset(content, ls);
    const size_t byte_offset = std::min(ls + static_cast<size_t>(byte_col_1based - 1), le);
    return utf16_units_for_utf8(std::string_view(content).substr(ls, le - ls), byte_offset - ls);
}

uint32_t lsp_character_for_byte_offset(const std::string& content,
                                       size_t line_start,
                                       size_t byte_offset) {
    const size_t le = next_line_offset(content, line_start);
    const size_t end = std::min(byte_offset, le);
    return utf16_units_for_utf8(std::string_view(content).substr(line_start, le - line_start),
                                end - line_start);
}

std::pair<uint32_t, uint32_t> to_lsp_position(const std::string& content,
                                              uint32_t line_1based,
                                              uint32_t byte_col_1based) {
    const uint32_t line = line_1based > 0 ? line_1based - 1 : 0;
    return {line, byte_column_to_lsp_character(content, line_1based, byte_col_1based)};
}

// Get the word at a given line/character position in source text.
std::string word_at_position(const std::string& content, int line, int character) {
    size_t ls = line_offset(content, line);
    size_t pos = lsp_position_to_offset(content, line, character);
    if (pos >= content.size()) {
        return "";
    }

    size_t start = pos;
    while (start > ls && is_ident_char(content[start - 1])) {
        --start;
    }
    size_t end = pos;
    while (end < content.size() && is_ident_char(content[end])) {
        ++end;
    }

    if (start == end) {
        return "";
    }
    return content.substr(start, end - start);
}

// Check if position is after a pipe operator '|>'
bool is_after_pipe(const std::string& content, int line, int character) {
    size_t pos = lsp_position_to_offset(content, line, character);
    if (pos < 2) {
        return false;
    }

    for (int i = static_cast<int>(pos) - 1; i >= 1; --i) {
        char c = content[static_cast<size_t>(i)];
        if (c == ' ' || c == '\t') {
            continue;
        }
        if (c == '>' && content[static_cast<size_t>(i - 1)] == '|') {
            return true;
        }
        break;
    }
    return false;
}

// Check if position is after a dot (member access / package qualifier)
std::string prefix_before_dot(const std::string& content, int line, int character) {
    size_t ls = line_offset(content, line);
    size_t pos = lsp_position_to_offset(content, line, character);
    if (pos == 0) {
        return "";
    }

    size_t i = pos;
    while (i > ls && is_ident_char(content[i - 1])) {
        --i;
    }
    if (i > ls && content[i - 1] == '.') {
        size_t dot = i - 1;
        size_t word_start = dot;
        while (word_start > ls && is_ident_char(content[word_start - 1])) {
            --word_start;
        }
        if (word_start < dot) {
            return content.substr(word_start, dot - word_start);
        }
    }
    return "";
}

// ============================================================
// Incremental text edit application
// ============================================================

// Apply an incremental text change (LSP TextDocumentContentChangeEvent with range).
// Returns true if applied, false if it's a full-content change.
bool apply_incremental_change(std::string& content,
                              int start_line,
                              int start_char,
                              int end_line,
                              int end_char,
                              const std::string& new_text) {
    // LSP character offsets are UTF-16 code units, while the buffer is UTF-8.
    size_t start_offset = lsp_position_to_offset(content, start_line, start_char);
    size_t end_offset = lsp_position_to_offset(content, end_line, end_char);

    if (start_offset > content.size()) {
        start_offset = content.size();
    }
    if (end_offset > content.size()) {
        end_offset = content.size();
    }
    if (start_offset > end_offset) {
        return false;
    }

    content.replace(start_offset, end_offset - start_offset, new_text);
    return true;
}

// ============================================================
// Built-in knowledge base
// ============================================================

const std::vector<std::pair<std::string, std::string>>& universe_table_builtins() {
    static const auto builtins = [] {
        std::vector<std::pair<std::string, std::string>> items;
        for (const auto& sig : analysis::BuiltinsForPackage("")) {
            items.emplace_back(sig->name, analysis::SignatureDetail(*sig));
        }
        return items;
    }();
    return builtins;
}

const std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>&
known_packages() {
    static const auto pkgs = [] {
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> items;
        for (const auto& package : analysis::KnownPackages()) {
            auto builtins = analysis::BuiltinsForPackage(package);
            auto& entries = items[package];
            entries.reserve(builtins.size());
            for (const auto* sig : builtins) {
                entries.emplace_back(sig->name, analysis::SignatureDetail(*sig));
            }
        }
        return items;
    }();
    return pkgs;
}

std::string percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(text[i + 1]) &&
            std::isxdigit(text[i + 2])) {
            const auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') {
                    return ch - '0';
                }
                if (ch >= 'a' && ch <= 'f') {
                    return ch - 'a' + 10;
                }
                return ch - 'A' + 10;
            };
            out.push_back(static_cast<char>((hex(text[i + 1]) << 4) | hex(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string file_uri_base_dir(const std::string& uri) {
    constexpr std::string_view prefix = "file://";
    if (!uri.starts_with(prefix)) {
        return "";
    }
    auto path = percent_decode(std::string_view(uri).substr(prefix.size()));
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) {
        return "";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::string package_path_for_qualifier(const analysis::AnalysisResult& analysis,
                                       const std::string& qualifier) {
    for (const auto& def : analysis.definitions) {
        if (def.kind == analysis::SymbolKind::Import && def.name == qualifier &&
            def.import_path.has_value()) {
            return *def.import_path;
        }
    }
    if (analysis::IsKnownPackage(qualifier)) {
        return qualifier;
    }
    return "";
}

const std::vector<std::string>& flux_keywords() {
    static const std::vector<std::string> kws = {
        "and",
        "or",
        "not",
        "if",
        "then",
        "else",
        "import",
        "package",
        "option",
        "builtin",
        "return",
        "testcase",
        "exists",
        "with",
        "extends",
        "true",
        "false",
    };
    return kws;
}

std::string trim_copy(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

std::string snippet_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '$' || c == '}' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

std::string make_function_snippet(const std::string& label,
                                  const std::vector<std::string>& params) {
    std::string snippet = snippet_escape(label);
    snippet += '(';
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) {
            snippet += ", ";
        }
        const auto param = trim_copy(params[i]);
        const auto index = std::to_string(i + 1);
        if (!param.empty() && param.back() == ':') {
            snippet += snippet_escape(param);
            snippet += " ${";
            snippet += index;
            snippet += ":value}";
        } else {
            snippet += "${";
            snippet += index;
            snippet += ':';
            snippet += snippet_escape(param.empty() ? "value" : param);
            snippet += '}';
        }
    }
    snippet += ")$0";
    return snippet;
}

} // namespace

// ============================================================
// FluxLanguageServer implementation
// ============================================================

FluxLanguageServer::FluxLanguageServer(StdioTransport transport, ServerOptions opts)
    : transport_(std::move(transport)), opts_(opts) {}

void FluxLanguageServer::run() {
    while (!shutdown_requested_) {
        auto message = transport_.read_message();
        if (!message) {
            break; // EOF
        }

        auto parsed = parse_message(*message);
        if (!parsed) {
            continue; // Malformed message, skip
        }

        dispatch(*parsed);
    }
}

void FluxLanguageServer::dispatch(const JsonRpcMessage& msg) {
    if (msg.method == "initialize") {
        handle_initialize(msg);
        return;
    }
    if (msg.method == "initialized") {
        handle_initialized(msg);
        return;
    }
    if (msg.method == "exit") {
        handle_exit(msg);
        return;
    }

    if (!initialized_) {
        if (msg.id) {
            reply_error(msg, ErrorCode::ServerNotInitialized, "Server not yet initialized");
        }
        return;
    }

    if (msg.method == "shutdown") {
        handle_shutdown(msg);
    } else if (msg.method == "textDocument/didOpen") {
        handle_did_open(msg);
    } else if (msg.method == "textDocument/didChange") {
        handle_did_change(msg);
    } else if (msg.method == "textDocument/didClose") {
        handle_did_close(msg);
    } else if (msg.method == "textDocument/completion") {
        handle_completion(msg);
    } else if (msg.method == "textDocument/hover") {
        handle_hover(msg);
    } else if (msg.method == "textDocument/formatting") {
        handle_formatting(msg);
    } else if (msg.method == "textDocument/documentSymbol") {
        handle_document_symbol(msg);
    } else if (msg.method == "textDocument/foldingRange") {
        handle_folding_range(msg);
    } else if (msg.method == "textDocument/definition") {
        handle_definition(msg);
    } else if (msg.method == "textDocument/references") {
        handle_references(msg);
    } else if (msg.method == "textDocument/rename") {
        handle_rename(msg);
    } else if (msg.method == "textDocument/signatureHelp") {
        handle_signature_help(msg);
    } else if (msg.method == "textDocument/documentHighlight") {
        handle_document_highlight(msg);
    } else if (msg.method == "textDocument/semanticTokens/full") {
        handle_semantic_tokens(msg);
    } else if (msg.method == "textDocument/codeAction") {
        handle_code_action(msg);
    } else if (msg.method == "textDocument/inlayHint") {
        handle_inlay_hint(msg);
    } else if (msg.method == "textDocument/selectionRange") {
        handle_selection_range(msg);
    } else {
        if (msg.id) {
            reply_error(msg, ErrorCode::MethodNotFound, "Method not found: " + msg.method);
        }
    }
}

void FluxLanguageServer::handle_initialize(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc = json_parser.parse(msg.params);
    if (!doc.error()) {
        auto max_width = doc.value().at_pointer("/initializationOptions/maxLineWidth");
        if (!max_width.error()) {
            auto n = max_width.get_int64();
            if (!n.error() && n.value() > 0) {
                opts_.format.max_line_width = static_cast<int>(n.value());
            }
        }
        auto indent_w = doc.value().at_pointer("/initializationOptions/indentWidth");
        if (!indent_w.error()) {
            auto n = indent_w.get_int64();
            if (!n.error() && n.value() > 0) {
                opts_.format.indent_width = static_cast<int>(n.value());
            }
        }
        auto use_tabs = doc.value().at_pointer("/initializationOptions/useTabs");
        if (!use_tabs.error()) {
            auto b = use_tabs.get_bool();
            if (!b.error()) {
                opts_.format.use_tabs = b.value();
            }
        }
    }

    // Server capabilities
    std::string result = R"({
  "capabilities": {
    "textDocumentSync": {
      "openClose": true,
      "change": 2
    },
    "completionProvider": {
      "triggerCharacters": [".", "|"],
      "resolveProvider": false
    },
    "hoverProvider": true,
    "documentFormattingProvider": true,
    "documentSymbolProvider": true,
    "foldingRangeProvider": true,
    "definitionProvider": true,
    "referencesProvider": true,
    "renameProvider": {"prepareProvider": false},
    "signatureHelpProvider": {
      "triggerCharacters": ["(", ","]
    },
    "documentHighlightProvider": true,
    "semanticTokensProvider": {
      "legend": {
        "tokenTypes": ["variable", "function", "keyword", "string", "number", "operator", "parameter", "type", "comment", "namespace"],
        "tokenModifiers": ["declaration", "definition", "readonly"]
      },
      "full": true
    },
    "codeActionProvider": true,
    "inlayHintProvider": true,
    "selectionRangeProvider": true
  },
  "serverInfo": {
    "name": "flux-ls",
    "version": "0.4.0"
  }
})";
    reply(msg, result);
}

void FluxLanguageServer::handle_initialized(const JsonRpcMessage& /*msg*/) {
    initialized_ = true;
}

void FluxLanguageServer::handle_shutdown(const JsonRpcMessage& msg) {
    shutdown_requested_ = true;
    reply(msg, "null");
}

void FluxLanguageServer::handle_exit(const JsonRpcMessage& /*msg*/) {
    shutdown_requested_ = true;
}

void FluxLanguageServer::handle_did_open(const JsonRpcMessage& msg) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(msg.params);
    if (doc.error()) {
        return;
    }

    auto uri = get_string_field(doc.value(), "/textDocument/uri");
    auto text = get_string_field(doc.value(), "/textDocument/text");

    Document& d = documents_[uri];
    d.uri = uri;
    d.content = text;
    d.version = 0;
    d.ast_version = -1; // force re-parse

    publish_diagnostics(uri);
}

void FluxLanguageServer::handle_did_change(const JsonRpcMessage& msg) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(msg.params);
    if (doc.error()) {
        return;
    }

    auto uri = get_string_field(doc.value(), "/textDocument/uri");

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        return;
    }

    auto changes = doc["contentChanges"];
    if (changes.error()) {
        return;
    }
    auto arr = changes.get_array();
    if (arr.error()) {
        return;
    }

    // Apply each change — supports both incremental (with range) and full content
    for (auto change : arr.value()) {
        auto text_val = change["text"];
        if (text_val.error()) {
            continue;
        }
        auto s = text_val.get_string();
        if (s.error()) {
            continue;
        }
        std::string new_text(s.value());

        // Check if this change has a range (incremental mode)
        auto range_val = change["range"];
        if (!range_val.error()) {
            auto sl = range_val["start"]["line"].get_int64();
            auto sc = range_val["start"]["character"].get_int64();
            auto el = range_val["end"]["line"].get_int64();
            auto ec = range_val["end"]["character"].get_int64();
            if (!sl.error() && !sc.error() && !el.error() && !ec.error()) {
                apply_incremental_change(it->second.content,
                                         static_cast<int>(sl.value()),
                                         static_cast<int>(sc.value()),
                                         static_cast<int>(el.value()),
                                         static_cast<int>(ec.value()),
                                         new_text);
            }
        } else {
            // Full content replacement
            it->second.content = new_text;
        }
    }

    it->second.version++;
    it->second.ast_version = -1; // invalidate cache

    publish_diagnostics(uri);
}

void FluxLanguageServer::handle_did_close(const JsonRpcMessage& msg) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(msg.params);
    if (doc.error()) {
        return;
    }

    auto uri = get_string_field(doc.value(), "/textDocument/uri");
    documents_.erase(uri);

    // Clear diagnostics
    std::ostringstream os;
    os << R"({"uri":)" << json_escape(uri) << R"(,"diagnostics":[]})";
    notify("textDocument/publishDiagnostics", os.str());
}

// ============================================================
// AST cache
// ============================================================

void FluxLanguageServer::ensure_ast(Document& doc) {
    if (doc.ast_version == doc.version) {
        return; // Cache is fresh
    }

    Parser parser(doc.content);
    auto file = parser.parse_file(doc.uri);
    doc.parse_errors = parser.errors();
    doc.ast = std::move(file);
    doc.ast_version = doc.version;
}

void FluxLanguageServer::ensure_analysis(Document& doc) {
    ensure_ast(doc);
    if (doc.analysis_version == doc.version) {
        return;
    }
    if (!doc.ast) {
        doc.analysis = analysis::AnalysisResult{};
        doc.analysis_version = doc.version;
        return;
    }
    doc.analysis = analysis::SemanticAnalyzer().Analyze(
        *doc.ast, {.source_base_dir = file_uri_base_dir(doc.uri)});
    doc.analysis_version = doc.version;
}

void FluxLanguageServer::publish_diagnostics(const std::string& uri) {
    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        return;
    }

    ensure_ast(it->second);
    ensure_analysis(it->second);
    const auto& doc = it->second;

    std::ostringstream os;
    os << R"({"uri":)" << json_escape(uri) << R"(,"diagnostics":[)";

    bool first = true;
    auto emit_diagnostic = [&](uint32_t start_line,
                               uint32_t start_col,
                               uint32_t end_line,
                               uint32_t end_col,
                               int severity,
                               const std::string& message) {
        if (!first) {
            os << ",";
        }
        first = false;
        // LSP Position is 0-based and uses UTF-16 code units.
        auto [sl, sc] = to_lsp_position(doc.content, start_line, start_col);
        auto [el, ec] = to_lsp_position(doc.content, end_line, end_col);
        os << R"({"range":{"start":{"line":)" << sl << R"(,"character":)" << sc
           << R"(},"end":{"line":)" << el << R"(,"character":)" << ec << R"(}},"severity":)"
           << severity << R"(,"source":"flux-ls","message":)" << json_escape(message) << "}";
    };

    // 从 AST 中收集 BadStatement 节点的精确位置
    if (doc.ast) {
        for (const auto& stmt : doc.ast->body) {
            if (stmt->type == Statement::Type::BadStatement) {
                const auto& loc = stmt->loc;
                const auto& bad = *std::get<std::unique_ptr<BadStmt>>(stmt->stmt);
                emit_diagnostic(loc.start.line,
                                loc.start.column,
                                loc.end.line,
                                loc.end.column,
                                1,
                                "syntax error: " + bad.text);
            }
        }
    }

    // parser 报出的全局错误
    for (const auto& err : doc.parse_errors) {
        emit_diagnostic(1, 1, 1, 1, 1, err);
    }

    if (doc.ast) {
        for (const auto& diag : doc.analysis.diagnostics) {
            emit_diagnostic(diag.location.start.line,
                            diag.location.start.column,
                            diag.location.end.line,
                            diag.location.end.column,
                            static_cast<int>(diag.severity),
                            diag.message);
        }
    }

    os << "]}";
    notify("textDocument/publishDiagnostics", os.str());
}

void FluxLanguageServer::handle_completion(const JsonRpcMessage& msg) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(msg.params);
    if (doc.error()) {
        reply(msg, R"({"isIncomplete":false,"items":[]})");
        return;
    }

    auto uri = get_string_field(doc.value(), "/textDocument/uri");
    auto line = get_int_field(doc.value(), "/position/line");
    auto character = get_int_field(doc.value(), "/position/character");

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, R"({"isIncomplete":false,"items":[]})");
        return;
    }

    auto result =
        build_completion_response(it->second, static_cast<int>(line), static_cast<int>(character));
    reply(msg, result);
}

void FluxLanguageServer::handle_hover(const JsonRpcMessage& msg) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(msg.params);
    if (doc.error()) {
        reply(msg, "null");
        return;
    }

    auto uri = get_string_field(doc.value(), "/textDocument/uri");
    auto line = get_int_field(doc.value(), "/position/line");
    auto character = get_int_field(doc.value(), "/position/character");

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "null");
        return;
    }

    ensure_analysis(it->second);
    auto result =
        build_hover_response(it->second, static_cast<int>(line), static_cast<int>(character));
    reply(msg, result);
}

std::string FluxLanguageServer::build_completion_response(Document& doc, int line, int character) {
    std::ostringstream os;
    os << R"({"isIncomplete":false,"items":[)";

    // 提取当前输入的 prefix 用于过滤候选项
    auto typing_prefix = word_at_position(doc.content, line, character);

    bool first = true;
    auto add_item = [&](const std::string& label,
                        const std::string& detail,
                        int kind,
                        const std::string& snippet) {
        // prefix 过滤
        if (!typing_prefix.empty() && !label.starts_with(typing_prefix)) {
            return;
        }
        if (!first) {
            os << ",";
        }
        first = false;
        os << R"({"label":)" << json_escape(label) << R"(,"kind":)" << kind << R"(,"detail":)"
           << json_escape(detail);
        if (!snippet.empty()) {
            os << R"(,"insertText":)" << json_escape(snippet) << R"(,"insertTextFormat":2)";
        }
        os << "}";
    };
    auto add_plain_item = [&](const std::string& label, const std::string& detail, int kind) {
        add_item(label, detail, kind, "");
    };
    auto add_function_item =
        [&](const std::string& label, const std::string& detail, std::vector<std::string> params) {
            add_item(label, detail, 3 /* Function */, make_function_snippet(label, params));
        };

    // Check context
    auto pkg_prefix = prefix_before_dot(doc.content, line, character);

    if (!pkg_prefix.empty()) {
        // Package member completion
        ensure_analysis(doc);
        const auto package_path = package_path_for_qualifier(doc.analysis, pkg_prefix);
        if (!package_path.empty()) {
            for (const auto* sig : analysis::BuiltinsForPackage(package_path)) {
                if (sig != nullptr) {
                    if (analysis::IsCallableBuiltin(*sig)) {
                        add_function_item(sig->name,
                                          analysis::SignatureDetail(*sig),
                                          analysis::CompletionParams(*sig));
                    } else {
                        add_plain_item(sig->name, analysis::SignatureDetail(*sig), 12 /* Value */);
                    }
                }
            }
        }
    } else if (is_after_pipe(doc.content, line, character)) {
        // After |> : suggest table transforms
        for (const auto* sig : analysis::BuiltinsForPackage("")) {
            if (sig != nullptr) {
                add_function_item(
                    sig->name, analysis::SignatureDetail(*sig), analysis::CompletionParams(*sig));
            }
        }
    } else {
        // General completion: keywords + universe builtins + known packages
        for (const auto& kw : flux_keywords()) {
            add_plain_item(kw, "keyword", 14 /* Keyword */);
        }
        for (const auto* sig : analysis::BuiltinsForPackage("")) {
            if (sig != nullptr) {
                add_function_item(
                    sig->name, analysis::SignatureDetail(*sig), analysis::CompletionParams(*sig));
            }
        }
        // Package names
        for (const auto& [pkg_name, _] : known_packages()) {
            add_plain_item(pkg_name, "package", 9 /* Module */);
        }

        // User-defined symbols from semantic analysis.
        ensure_analysis(doc);
        for (const auto& def : doc.analysis.definitions) {
            if (def.kind == analysis::SymbolKind::Parameter ||
                def.kind == analysis::SymbolKind::Import) {
                continue;
            }
            switch (def.kind) {
                case analysis::SymbolKind::Function:
                    add_function_item(def.name, def.type.ToString(), def.parameters);
                    break;
                case analysis::SymbolKind::Builtin:
                case analysis::SymbolKind::PackageBuiltin:
                    add_function_item(def.name, "builtin", def.parameters);
                    break;
                case analysis::SymbolKind::Option:
                    add_plain_item(def.name, "option: " + def.type.ToString(), 6 /* Variable */);
                    break;
                default:
                    add_plain_item(def.name, def.type.ToString(), 6 /* Variable */);
                    break;
            }
        }
    }

    os << "]}";
    return os.str();
}

std::string FluxLanguageServer::build_hover_response(const Document& doc, int line, int character) {
    auto word = word_at_position(doc.content, line, character);
    if (word.empty()) {
        return "null";
    }

    const auto ast_line = static_cast<uint32_t>(line + 1);
    const auto ast_col = lsp_position_to_byte_column(doc.content, line, character);
    if (const auto* def = doc.analysis.DefinitionForSymbolAt(ast_line, ast_col); def != nullptr) {
        std::ostringstream os;
        std::string hover_text = "**" + def->name + "**\n\n`" + def->type.ToString() + "`";
        os << R"({"contents":{"kind":"markdown","value":)" << json_escape(hover_text) << "}}";
        return os.str();
    }

    // Check if it's a builtin
    for (const auto& [name, detail] : universe_table_builtins()) {
        if (name == word) {
            std::ostringstream os;
            std::string hover_text;
            hover_text.reserve(name.size() + detail.size() + 10);
            hover_text += "**";
            hover_text += name;
            hover_text += "**\n\n";
            hover_text += detail;
            os << R"({"contents":{"kind":"markdown","value":)" << json_escape(hover_text) << "}}";
            return os.str();
        }
    }

    // Check packages
    auto pkg_prefix = prefix_before_dot(doc.content, line, character);
    if (!pkg_prefix.empty()) {
        const auto package_path = package_path_for_qualifier(doc.analysis, pkg_prefix);
        const auto& pkgs = known_packages();
        auto pkg_it = pkgs.find(package_path);
        if (pkg_it != pkgs.end()) {
            for (const auto& [name, detail] : pkg_it->second) {
                if (name == word) {
                    std::ostringstream os;
                    std::string hover_text;
                    hover_text.reserve(pkg_prefix.size() + name.size() + detail.size() + 16);
                    hover_text += "**";
                    hover_text += pkg_prefix;
                    hover_text += '.';
                    hover_text += name;
                    hover_text += "**\n\n";
                    hover_text += detail;
                    os << R"({"contents":{"kind":"markdown","value":)" << json_escape(hover_text)
                       << "}}";
                    return os.str();
                }
            }
        }
    }

    if (auto type = doc.analysis.TypeAt(ast_line, ast_col); type.has_value()) {
        std::ostringstream os;
        os << R"({"contents":{"kind":"markdown","value":)"
           << json_escape("`" + type->ToString() + "`") << "}}";
        return os.str();
    }

    // Check if it's a keyword
    for (const auto& kw : flux_keywords()) {
        if (kw == word) {
            std::ostringstream os;
            os << R"({"contents":{"kind":"markdown","value":)"
               << json_escape("**" + word + "** (keyword)") << "}}";
            return os.str();
        }
    }

    return "null";
}

void FluxLanguageServer::handle_document_symbol(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "[]");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "[]");
        return;
    }

    ensure_ast(it->second);
    const auto& doc = it->second;
    if (!doc.ast) {
        reply(msg, "[]");
        return;
    }

    std::ostringstream os;
    os << "[";
    bool first = true;

    auto emit_symbol = [&](const std::string& name, int kind, const SourceLocation& loc) {
        if (!first) {
            os << ",";
        }
        first = false;
        // SymbolKind: 12=Function, 13=Variable, 14=Constant, 2=Module, 3=Namespace
        auto [sl, sc] = to_lsp_position(doc.content, loc.start.line, loc.start.column);
        auto [el, ec] = to_lsp_position(doc.content, loc.end.line, loc.end.column);
        os << R"({"name":)" << json_escape(name) << R"(,"kind":)" << kind
           << R"(,"range":{"start":{"line":)" << sl << R"(,"character":)" << sc
           << R"(},"end":{"line":)" << el << R"(,"character":)" << ec
           << R"(}},"selectionRange":{"start":{"line":)" << sl << R"(,"character":)" << sc
           << R"(},"end":{"line":)" << el << R"(,"character":)" << ec << "}}}";
    };

    // Package clause
    if (doc.ast->package && doc.ast->package->name) {
        emit_symbol(doc.ast->package->name->name, 4 /* Package */, doc.ast->package->loc);
    }

    // Imports
    for (const auto& imp : doc.ast->imports) {
        std::string label;
        if (imp->alias && !imp->alias->name.empty()) {
            label = imp->alias->name;
        } else if (imp->path) {
            label = imp->path->value;
        }
        if (!label.empty()) {
            emit_symbol(label, 2 /* Module */, imp->loc);
        }
    }

    // Body statements
    for (const auto& stmt : doc.ast->body) {
        if (stmt->type == Statement::Type::VariableAssignment) {
            const auto& va = *std::get<std::unique_ptr<VariableAssgn>>(stmt->stmt);
            if (va.id && !va.id->name.empty()) {
                int kind = 13; // Variable
                if (va.init && va.init->type == Expression::Type::FunctionExpr) {
                    kind = 12; // Function
                }
                emit_symbol(va.id->name, kind, stmt->loc);
            }
        } else if (stmt->type == Statement::Type::OptionStatement) {
            const auto& opt = *std::get<std::unique_ptr<OptionStmt>>(stmt->stmt);
            if (opt.assignment && opt.assignment->type == Assignment::Type::VariableAssignment) {
                const auto& va = *std::get<std::unique_ptr<VariableAssgn>>(opt.assignment->value);
                if (va.id && !va.id->name.empty()) {
                    emit_symbol("option " + va.id->name, 14 /* Constant */, stmt->loc);
                }
            }
        } else if (stmt->type == Statement::Type::BuiltinStatement) {
            const auto& bi = *std::get<std::unique_ptr<BuiltinStmt>>(stmt->stmt);
            if (bi.id && !bi.id->name.empty()) {
                emit_symbol(bi.id->name, 12 /* Function */, stmt->loc);
            }
        } else if (stmt->type == Statement::Type::TestCaseStatement) {
            const auto& tc = *std::get<std::unique_ptr<TestCaseStmt>>(stmt->stmt);
            if (tc.id && !tc.id->name.empty()) {
                emit_symbol(tc.id->name, 12 /* Function */, stmt->loc);
            }
        }
    }

    os << "]";
    reply(msg, os.str());
}

void FluxLanguageServer::handle_folding_range(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "[]");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "[]");
        return;
    }

    ensure_ast(it->second);
    const auto& doc = it->second;
    if (!doc.ast) {
        reply(msg, "[]");
        return;
    }

    std::ostringstream os;
    os << "[";
    bool first = true;

    auto emit_range = [&](uint32_t start_line, uint32_t end_line, const std::string& kind) {
        // LSP folding range uses 0-based lines; only emit if range spans >1 line
        uint32_t sl = start_line > 0 ? start_line - 1 : 0;
        uint32_t el = end_line > 0 ? end_line - 1 : 0;
        if (el <= sl) {
            return;
        }
        if (!first) {
            os << ",";
        }
        first = false;
        os << R"({"startLine":)" << sl << R"(,"endLine":)" << el;
        if (!kind.empty()) {
            os << R"(,"kind":)" << json_escape(kind);
        }
        os << "}";
    };

    // Collect folding ranges from top-level statements that span multiple lines
    for (const auto& stmt : doc.ast->body) {
        const auto& loc = stmt->loc;
        if (!loc.is_valid()) {
            continue;
        }

        if (stmt->type == Statement::Type::VariableAssignment) {
            const auto& va = *std::get<std::unique_ptr<VariableAssgn>>(stmt->stmt);
            // Functions and multi-line expressions are foldable
            if (va.init) {
                const bool is_foldable_expr = va.init->type == Expression::Type::FunctionExpr ||
                                              va.init->type == Expression::Type::ObjectExpr ||
                                              va.init->type == Expression::Type::ArrayExpr ||
                                              va.init->type == Expression::Type::PipeExpr;
                if (is_foldable_expr || loc.end.line > loc.start.line) {
                    emit_range(loc.start.line, loc.end.line, "region");
                }
            }
        } else if (stmt->type == Statement::Type::OptionStatement ||
                   stmt->type == Statement::Type::TestCaseStatement ||
                   stmt->type == Statement::Type::ExpressionStatement) {
            emit_range(loc.start.line, loc.end.line, "region");
        }
    }

    // Import block: if there are multiple imports, fold them as a group
    if (doc.ast->imports.size() > 1) {
        uint32_t imp_start = doc.ast->imports.front()->loc.start.line;
        uint32_t imp_end = doc.ast->imports.back()->loc.end.line;
        emit_range(imp_start, imp_end, "imports");
    }

    os << "]";
    reply(msg, os.str());
}

// ============================================================
// Phase 2: Semantic navigation handlers
// ============================================================

void FluxLanguageServer::handle_definition(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "null");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto line = get_int_field(doc_result.value(), "/position/line");
    auto character = get_int_field(doc_result.value(), "/position/character");

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "null");
        return;
    }

    ensure_analysis(it->second);
    const auto& analysis = it->second.analysis;
    const auto& content = it->second.content;

    const auto ast_line = static_cast<uint32_t>(line + 1);
    const auto ast_col =
        lsp_position_to_byte_column(content, static_cast<int>(line), static_cast<int>(character));
    const auto* def = analysis.DefinitionForSymbolAt(ast_line, ast_col);
    if (!def) {
        reply(msg, "null");
        return;
    }

    auto [sl, sc] =
        to_lsp_position(it->second.content, def->location.start.line, def->location.start.column);
    auto [el, ec] =
        to_lsp_position(it->second.content, def->location.end.line, def->location.end.column);

    std::ostringstream os;
    os << R"({"uri":)" << json_escape(uri) << R"(,"range":{"start":{"line":)" << sl
       << R"(,"character":)" << sc << R"(},"end":{"line":)" << el << R"(,"character":)" << ec
       << "}}}";
    reply(msg, os.str());
}

void FluxLanguageServer::handle_references(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "[]");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto line = get_int_field(doc_result.value(), "/position/line");
    auto character = get_int_field(doc_result.value(), "/position/character");

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "[]");
        return;
    }

    ensure_analysis(it->second);
    const auto& analysis = it->second.analysis;
    const auto& content = it->second.content;

    auto include_decl_val = doc_result.value().at_pointer("/context/includeDeclaration");
    bool include_declaration = true;
    if (!include_decl_val.error()) {
        auto b = include_decl_val.get_bool();
        if (!b.error()) {
            include_declaration = b.value();
        }
    }

    const auto ast_line = static_cast<uint32_t>(line + 1);
    const auto ast_col =
        lsp_position_to_byte_column(content, static_cast<int>(line), static_cast<int>(character));
    const auto* target_def = analysis.DefinitionForSymbolAt(ast_line, ast_col);
    const auto fallback_name = analysis.SymbolAt(ast_line, ast_col);

    // Collect all references + optionally the definition itself
    std::ostringstream os;
    os << "[";
    bool first = true;

    auto emit_location = [&](const SourceLocation& loc) {
        if (!first) {
            os << ",";
        }
        first = false;
        auto [sl, sc] = to_lsp_position(content, loc.start.line, loc.start.column);
        auto [el, ec] = to_lsp_position(content, loc.end.line, loc.end.column);
        os << R"({"uri":)" << json_escape(uri) << R"(,"range":{"start":{"line":)" << sl
           << R"(,"character":)" << sc << R"(},"end":{"line":)" << el << R"(,"character":)" << ec
           << "}}})";
    };

    if (include_declaration && target_def) {
        emit_location(target_def->location);
    }

    // All references
    auto refs =
        target_def ? analysis.ReferencesOf(*target_def) : analysis.ReferencesOf(fallback_name);
    for (const auto* ref : refs) {
        emit_location(ref->location);
    }

    os << "]";
    reply(msg, os.str());
}

void FluxLanguageServer::handle_rename(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "null");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto line = get_int_field(doc_result.value(), "/position/line");
    auto character = get_int_field(doc_result.value(), "/position/character");
    auto new_name = get_string_field(doc_result.value(), "/newName");

    if (new_name.empty()) {
        reply_error(msg, ErrorCode::InvalidParams, "newName is required");
        return;
    }

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "null");
        return;
    }

    ensure_analysis(it->second);
    const auto& analysis = it->second.analysis;
    const auto& content = it->second.content;

    // Collect all locations that need to be renamed (definition + references)
    std::vector<SourceLocation> locations;

    const auto ast_line = static_cast<uint32_t>(line + 1);
    const auto ast_col =
        lsp_position_to_byte_column(content, static_cast<int>(line), static_cast<int>(character));
    const auto* def = analysis.DefinitionForSymbolAt(ast_line, ast_col);
    if (def) {
        locations.push_back(def->location);
    } else {
        reply(msg, "null");
        return;
    }

    auto refs = analysis.ReferencesOf(*def);
    for (const auto* ref : refs) {
        locations.push_back(ref->location);
    }

    bool has_valid_location = false;
    for (const auto& loc : locations) {
        if (loc.start.line > 0 && loc.start.column > 0) {
            has_valid_location = true;
            break;
        }
    }

    if (locations.empty() || !has_valid_location) {
        reply(msg, "null");
        return;
    }

    // Build WorkspaceEdit with TextEdits grouped by document URI
    std::ostringstream os;
    os << R"({"changes":{)" << json_escape(uri) << ":[";
    bool first = true;

    auto emit_edit = [&](uint32_t edit_line, uint32_t start_character, uint32_t end_character) {
        if (!first) {
            os << ",";
        }
        first = false;
        os << R"({"range":{"start":{"line":)" << edit_line << R"(,"character":)" << start_character
           << R"(},"end":{"line":)" << edit_line << R"(,"character":)" << end_character
           << R"(}},"newText":)" << json_escape(new_name) << "}";
    };

    if (has_valid_location) {
        for (const auto& loc : locations) {
            if (loc.start.line == 0 || loc.start.column == 0) {
                continue;
            }
            auto [sl, sc] = to_lsp_position(content, loc.start.line, loc.start.column);
            auto [el, ec] = to_lsp_position(content, loc.end.line, loc.end.column);
            if (sl == el) {
                emit_edit(sl, sc, ec);
            }
        }
    }
    os << "]}}";
    reply(msg, os.str());
}

void FluxLanguageServer::handle_signature_help(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "null");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto line = get_int_field(doc_result.value(), "/position/line");
    auto character = get_int_field(doc_result.value(), "/position/character");

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "null");
        return;
    }

    // Find the function name being called by scanning backwards for an identifier before '('
    const auto& content = it->second.content;
    size_t pos =
        lsp_position_to_offset(content, static_cast<int>(line), static_cast<int>(character));

    // Count commas to determine active parameter
    int active_param = 0;
    int paren_depth = 0;
    std::string func_name;
    std::string package_name;

    for (int i = static_cast<int>(pos) - 1; i >= 0; --i) {
        char c = content[static_cast<size_t>(i)];
        if (c == ')') {
            ++paren_depth;
        } else if (c == '(') {
            if (paren_depth > 0) {
                --paren_depth;
            } else {
                // Found the opening paren — extract the function name before it
                int name_end = i;
                while (name_end > 0 && content[static_cast<size_t>(name_end - 1)] == ' ') {
                    --name_end;
                }
                int name_start = name_end;
                while (name_start > 0 &&
                       is_ident_char(content[static_cast<size_t>(name_start - 1)])) {
                    --name_start;
                }
                if (name_start < name_end) {
                    func_name = content.substr(static_cast<size_t>(name_start),
                                               static_cast<size_t>(name_end - name_start));
                }
                int package_end = name_start;
                if (package_end > 0 && content[static_cast<size_t>(package_end - 1)] == '.') {
                    int package_start = package_end - 1;
                    while (package_start > 0 &&
                           is_ident_char(content[static_cast<size_t>(package_start - 1)])) {
                        --package_start;
                    }
                    if (package_start < package_end - 1) {
                        package_name =
                            content.substr(static_cast<size_t>(package_start),
                                           static_cast<size_t>(package_end - 1 - package_start));
                    }
                }
                break;
            }
        } else if (c == ',' && paren_depth == 0) {
            ++active_param;
        }
    }

    if (func_name.empty()) {
        reply(msg, "null");
        return;
    }

    // Look up the function in semantic analysis for parameter info.
    ensure_analysis(it->second);
    const auto& analysis = it->second.analysis;
    const auto* def = analysis.FindDefinition(func_name);

    std::vector<std::string> params;
    std::string label;
    if (package_name.empty() && def && !def->parameters.empty()) {
        params = def->parameters;
        label = func_name + "(";
    } else {
        const auto package_path = package_name.empty()
                                      ? std::string{}
                                      : package_path_for_qualifier(analysis, package_name);
        const auto* sig =
            package_name.empty()
                ? analysis::FindUniverseBuiltinSignature(func_name)
                : (package_path.empty() ? nullptr
                                        : analysis::FindBuiltinSignature(package_path, func_name));
        if (sig != nullptr) {
            params = analysis::CompletionParams(*sig);
            label = analysis::SignatureLabel(*sig);
        }
    }

    if (params.empty()) {
        reply(msg, "null");
        return;
    }

    // Build SignatureHelp response
    std::ostringstream os;
    if (label.empty() || label.back() == '(') {
        if (label.empty()) {
            label = func_name + "(";
        }
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) {
                label += ", ";
            }
            label += params[i];
        }
        label += ")";
    }
    os << R"({"signatures":[{"label":)" << json_escape(label) << R"(,"parameters":[)";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        os << R"({"label":)" << json_escape(params[i]) << "}";
    }
    os << R"(]}],"activeSignature":0,"activeParameter":)" << active_param << "}";
    reply(msg, os.str());
}

void FluxLanguageServer::handle_document_highlight(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "[]");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto line = get_int_field(doc_result.value(), "/position/line");
    auto character = get_int_field(doc_result.value(), "/position/character");

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "[]");
        return;
    }

    ensure_analysis(it->second);
    const auto& analysis = it->second.analysis;
    const auto& content = it->second.content;

    // Use word_at_position for precise cursor identification
    auto name =
        word_at_position(it->second.content, static_cast<int>(line), static_cast<int>(character));
    if (name.empty()) {
        uint32_t ast_line = static_cast<uint32_t>(line + 1);
        uint32_t ast_col = lsp_position_to_byte_column(
            it->second.content, static_cast<int>(line), static_cast<int>(character));
        name = analysis.SymbolAt(ast_line, ast_col);
    }
    if (name.empty()) {
        reply(msg, "[]");
        return;
    }

    // Highlight: definition (kind=2, Write) + all references (kind=1, Read)
    std::ostringstream os;
    os << "[";
    bool first = true;

    auto emit_highlight = [&](const SourceLocation& loc, int kind) {
        if (!first) {
            os << ",";
        }
        first = false;
        auto [sl, sc] = to_lsp_position(content, loc.start.line, loc.start.column);
        auto [el, ec] = to_lsp_position(content, loc.end.line, loc.end.column);
        os << R"({"range":{"start":{"line":)" << sl << R"(,"character":)" << sc
           << R"(},"end":{"line":)" << el << R"(,"character":)" << ec << R"(}},"kind":)" << kind
           << "}";
    };

    // Definition highlight (Write = 3)
    const auto* def = analysis.DefinitionForSymbolAt(
        static_cast<uint32_t>(line + 1),
        lsp_position_to_byte_column(content, static_cast<int>(line), static_cast<int>(character)));
    if (!def) {
        def = analysis.FindDefinition(name);
    }
    if (def) {
        emit_highlight(def->location, 3);
    }

    // Reference highlights (Read = 2)
    auto refs = def ? analysis.ReferencesOf(*def) : analysis.ReferencesOf(name);
    for (const auto* ref : refs) {
        emit_highlight(ref->location, 2);
    }

    os << "]";
    reply(msg, os.str());
}

// ============================================================
// Phase 3: Advanced features
// ============================================================

void FluxLanguageServer::handle_semantic_tokens(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, R"({"data":[]})");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, R"({"data":[]})");
        return;
    }

    ensure_analysis(it->second);
    const auto& doc = it->second;
    if (!doc.ast) {
        reply(msg, R"({"data":[]})");
        return;
    }

    // Token type indices (matching the legend in capabilities):
    // 0=variable, 1=function, 2=keyword, 3=string, 4=number,
    // 5=operator, 6=parameter, 7=type, 8=comment, 9=namespace
    //
    // Token modifier bitmask:
    // bit 0=declaration, bit 1=definition, bit 2=readonly

    // Collect tokens: (line, col, length, type, modifiers) all 1-based from AST
    struct SemanticToken {
        uint32_t line; // 1-based
        uint32_t col;  // 1-based
        uint32_t length;
        uint32_t type;
        uint32_t modifiers;
    };
    std::vector<SemanticToken> tokens;

    // Definitions
    for (const auto& def : doc.analysis.definitions) {
        uint32_t type_idx = 0; // variable
        uint32_t mod = 0b011;  // declaration + definition
        switch (def.kind) {
            case analysis::SymbolKind::Function:
                type_idx = 1;
                break;
            case analysis::SymbolKind::Variable:
                type_idx = 0;
                break;
            case analysis::SymbolKind::Option:
                type_idx = 0;
                mod = 0b111; // declaration + definition + readonly
                break;
            case analysis::SymbolKind::Builtin:
            case analysis::SymbolKind::PackageBuiltin:
                type_idx = 1;
                mod = 0b101; // declaration + readonly
                break;
            case analysis::SymbolKind::Import:
                type_idx = 9; // namespace
                break;
            case analysis::SymbolKind::Parameter:
                type_idx = 6;
                break;
        }
        if (def.location.start.line > 0 && def.name.size() > 0) {
            tokens.push_back({
                .line = def.location.start.line,
                .col = def.location.start.column,
                .length = static_cast<uint32_t>(def.name.size()),
                .type = type_idx,
                .modifiers = mod,
            });
        }
    }

    // References
    for (const auto& ref : doc.analysis.references) {
        // Determine type by looking up definition
        uint32_t type_idx = 0; // variable by default
        const auto* def = doc.analysis.FindDefinition(ref.definition_id);
        if (def) {
            switch (def->kind) {
                case analysis::SymbolKind::Function:
                case analysis::SymbolKind::Builtin:
                case analysis::SymbolKind::PackageBuiltin:
                    type_idx = 1;
                    break;
                case analysis::SymbolKind::Parameter:
                    type_idx = 6;
                    break;
                case analysis::SymbolKind::Import:
                    type_idx = 9;
                    break;
                default:
                    type_idx = 0;
                    break;
            }
        } else if (ref.kind == analysis::ReferenceKind::PackageMember) {
            type_idx = 1;
        }
        if (ref.location.start.line > 0 && ref.name.size() > 0) {
            tokens.push_back({
                .line = ref.location.start.line,
                .col = ref.location.start.column,
                .length = static_cast<uint32_t>(ref.name.size()),
                .type = type_idx,
                .modifiers = 0,
            });
        }
    }

    // Sort tokens by (line, col)
    std::sort(tokens.begin(), tokens.end(), [](const SemanticToken& a, const SemanticToken& b) {
        if (a.line != b.line) {
            return a.line < b.line;
        }
        return a.col < b.col;
    });

    // Encode as delta-encoded data array (LSP spec)
    // Each token is 5 integers: deltaLine, deltaStartChar, length, tokenType, tokenModifiers
    std::ostringstream os;
    os << R"({"data":[)";
    uint32_t prev_line = 0;
    uint32_t prev_col = 0;
    bool first = true;
    for (const auto& tok : tokens) {
        // Convert to 0-based for delta encoding
        uint32_t line_0 = tok.line - 1;
        uint32_t col_0 = byte_column_to_lsp_character(doc.content, tok.line, tok.col);

        uint32_t delta_line = line_0 - prev_line;
        uint32_t delta_col = (delta_line == 0) ? (col_0 - prev_col) : col_0;

        if (!first) {
            os << ",";
        }
        first = false;
        os << delta_line << "," << delta_col << "," << tok.length << "," << tok.type << ","
           << tok.modifiers;

        prev_line = line_0;
        prev_col = col_0;
    }
    os << "]}";
    reply(msg, os.str());
}

void FluxLanguageServer::handle_code_action(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "[]");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "[]");
        return;
    }

    ensure_analysis(it->second);
    const auto& doc = it->second;

    // Provide code actions for semantic diagnostics (undefined identifiers)
    // Generate "Add import" suggestions for undefined identifiers that look like package names
    auto range_start_line = get_int_field(doc_result.value(), "/range/start/line");
    auto range_start_char = get_int_field(doc_result.value(), "/range/start/character");
    auto range_end_line = get_int_field(doc_result.value(), "/range/end/line");
    auto range_end_char = get_int_field(doc_result.value(), "/range/end/character");

    std::ostringstream os;
    os << "[";
    bool first = true;

    // Check if there are diagnostics in the requested range that we can fix
    for (const auto& diag : doc.analysis.diagnostics) {
        auto [diag_line, diag_col] =
            to_lsp_position(doc.content, diag.location.start.line, diag.location.start.column);

        // Check if diagnostic falls within the requested range
        if (static_cast<int64_t>(diag_line) >= range_start_line &&
            static_cast<int64_t>(diag_line) <= range_end_line) {
            // Extract the identifier name from the diagnostic message
            const std::string prefix = "undefined identifier: ";
            if (diag.message.find(prefix) == 0) {
                std::string id_name = diag.message.substr(prefix.size());
                if (const auto semicolon = id_name.find(';'); semicolon != std::string::npos) {
                    id_name = id_name.substr(0, semicolon);
                }

                // Check if it looks like a known package name
                const auto& pkgs = known_packages();
                if (pkgs.count(id_name) > 0) {
                    if (!first) {
                        os << ",";
                    }
                    first = false;

                    // Insert at line 0, character 0 (beginning of file)
                    os << R"({"title":"Add import for ')" << id_name
                       << R"('","kind":"quickfix","edit":{"changes":{)" << json_escape(uri)
                       << R"(:[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"newText":)"
                       << json_escape("import \"" + id_name + "\"\n") << "}]}}})";
                }
            }
        }
        (void)diag_col;
        (void)range_start_char;
        (void)range_end_char;
    }

    os << "]";
    reply(msg, os.str());
}

void FluxLanguageServer::handle_inlay_hint(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "[]");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "[]");
        return;
    }

    ensure_ast(it->second);
    const auto& doc = it->second;
    if (!doc.ast) {
        reply(msg, "[]");
        return;
    }

    // Get the requested range
    auto range_start_line = get_int_field(doc_result.value(), "/range/start/line");
    auto range_end_line = get_int_field(doc_result.value(), "/range/end/line");

    // Inlay hints: show parameter names at call sites
    // We traverse the AST looking for CallExpr nodes with named arguments
    // For positional args (ObjectExpr properties in arguments), show the parameter name
    std::ostringstream os;
    os << "[";
    bool first = true;

    // Helper: scan all expression statements and variable assignments for call expressions
    // For each call, if the function has known parameters, show inlay hints for arguments
    auto emit_hint = [&](uint32_t line_1based, uint32_t col_1based, const std::string& label) {
        auto [line_0, col_0] = to_lsp_position(doc.content, line_1based, col_1based);

        // Only emit if within requested range
        if (static_cast<int64_t>(line_0) < range_start_line ||
            static_cast<int64_t>(line_0) > range_end_line) {
            return;
        }

        if (!first) {
            os << ",";
        }
        first = false;
        os << R"({"position":{"line":)" << line_0 << R"(,"character":)" << col_0 << R"(},"label":)"
           << json_escape(label + ":") << R"(,"kind":2,"paddingRight":true})";
    };

    // For simplicity, provide inlay hints for function calls where we know the parameter names
    // Traverse the symbol table references looking for call patterns
    // Since we don't have a full call-site AST walk here, let's provide hints for
    // user-defined function calls by matching the function parameters
    // This is a simplified implementation - a full implementation would walk the AST
    for (const auto& stmt : doc.ast->body) {
        if (stmt->type == Statement::Type::VariableAssignment) {
            const auto& va = *std::get<std::unique_ptr<VariableAssgn>>(stmt->stmt);
            if (va.init && va.init->type == Expression::Type::FunctionExpr) {
                // This is a function definition: show parameter types as hints
                const auto& fn = *std::get<std::unique_ptr<FunctionExpr>>(va.init->expr);
                for (const auto& p : fn.params) {
                    if (p->value && p->key) {
                        // Has a default value — show the default as a hint
                        if (p->key->type == PropertyKey::Type::Identifier) {
                            const auto& id = *std::get<std::unique_ptr<Identifier>>(p->key->key);
                            if (p->value->type == Expression::Type::IntegerLit) {
                                emit_hint(p->loc.end.line,
                                          p->loc.end.column,
                                          " = " +
                                              std::to_string(std::get<std::unique_ptr<IntegerLit>>(
                                                                 p->value->expr)
                                                                 ->value));
                            } else if (p->value->type == Expression::Type::BooleanLit) {
                                emit_hint(p->loc.end.line,
                                          p->loc.end.column,
                                          std::string(" = ") +
                                              (std::get<std::unique_ptr<BooleanLit>>(p->value->expr)
                                                       ->value
                                                   ? "true"
                                                   : "false"));
                            }
                            (void)id;
                        }
                    }
                }
            }
        }
    }

    os << "]";
    reply(msg, os.str());
}

void FluxLanguageServer::handle_selection_range(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "[]");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");
    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "[]");
        return;
    }

    ensure_ast(it->second);
    const auto& doc = it->second;
    if (!doc.ast) {
        reply(msg, "[]");
        return;
    }

    // Parse positions array
    auto positions = doc_result.value().at_pointer("/positions");
    if (positions.error()) {
        reply(msg, "[]");
        return;
    }
    auto pos_arr = positions.get_array();
    if (pos_arr.error()) {
        reply(msg, "[]");
        return;
    }

    std::ostringstream os;
    os << "[";
    bool first_pos = true;

    for (auto pos_elem : pos_arr.value()) {
        auto line_val = pos_elem["line"].get_int64();
        auto char_val = pos_elem["character"].get_int64();
        if (line_val.error() || char_val.error()) {
            continue;
        }

        int64_t line = line_val.value();      // 0-based
        int64_t character = char_val.value(); // 0-based
        uint32_t ast_line = static_cast<uint32_t>(line + 1);
        uint32_t ast_col = lsp_position_to_byte_column(
            doc.content, static_cast<int>(line), static_cast<int>(character));

        if (!first_pos) {
            os << ",";
        }
        first_pos = false;

        // Find the narrowest AST node that contains this position, building a hierarchy
        // Strategy: find the innermost statement, then find enclosing blocks
        // For simplicity, we create a 2-level selection:
        //   1. The word at cursor (innermost)
        //   2. The enclosing statement (outer)

        // Inner range: the word under cursor
        auto word =
            word_at_position(doc.content, static_cast<int>(line), static_cast<int>(character));
        uint32_t word_start_col = static_cast<uint32_t>(character);
        uint32_t word_end_col = static_cast<uint32_t>(character);
        if (!word.empty()) {
            // Find the actual start of the word
            size_t ls = line_offset(doc.content, static_cast<int>(line));
            size_t cursor = lsp_position_to_offset(
                doc.content, static_cast<int>(line), static_cast<int>(character));
            size_t ws = cursor;
            while (ws > ls && is_ident_char(doc.content[ws - 1])) {
                --ws;
            }
            word_start_col = lsp_character_for_byte_offset(doc.content, ls, ws);
            word_end_col = lsp_character_for_byte_offset(doc.content, ls, ws + word.size());
        }

        // Find the enclosing statement
        const SourceLocation* stmt_loc = nullptr;
        for (const auto& stmt : doc.ast->body) {
            if (stmt->loc.start.line <= ast_line && stmt->loc.end.line >= ast_line) {
                if (ast_line > stmt->loc.start.line || ast_col >= stmt->loc.start.column) {
                    if (ast_line < stmt->loc.end.line || ast_col <= stmt->loc.end.column) {
                        stmt_loc = &stmt->loc;
                    }
                }
            }
        }

        // Build selection range: inner (word) -> outer (statement)
        if (stmt_loc) {
            auto [stmt_sl, stmt_sc] =
                to_lsp_position(doc.content, stmt_loc->start.line, stmt_loc->start.column);
            auto [stmt_el, stmt_ec] =
                to_lsp_position(doc.content, stmt_loc->end.line, stmt_loc->end.column);

            os << R"({"range":{"start":{"line":)" << line << R"(,"character":)" << word_start_col
               << R"(},"end":{"line":)" << line << R"(,"character":)" << word_end_col
               << R"(}},"parent":{"range":{"start":{"line":)" << stmt_sl << R"(,"character":)"
               << stmt_sc << R"(},"end":{"line":)" << stmt_el << R"(,"character":)" << stmt_ec
               << "}}}})";
        } else {
            // No statement found, just return the word range
            os << R"({"range":{"start":{"line":)" << line << R"(,"character":)" << word_start_col
               << R"(},"end":{"line":)" << line << R"(,"character":)" << word_end_col << "}}}";
        }
    }

    os << "]";
    reply(msg, os.str());
}

void FluxLanguageServer::handle_formatting(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc_result = json_parser.parse(msg.params);
    if (doc_result.error()) {
        reply(msg, "[]");
        return;
    }

    auto uri = get_string_field(doc_result.value(), "/textDocument/uri");

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "[]");
        return;
    }

    ensure_ast(it->second);
    const auto& doc = it->second;

    // Only format if there are no parse errors
    if (!doc.parse_errors.empty() || !doc.ast) {
        reply(msg, "[]");
        return;
    }

    // Start with server-level format options, then override with per-request options.
    Formatter::Options fmt_opts = opts_.format;
    auto tab_size = get_int_field(doc_result.value(), "/options/tabSize");
    if (tab_size > 0) {
        fmt_opts.indent_width = static_cast<int>(tab_size);
    }
    auto insert_spaces_val = doc_result.value().at_pointer("/options/insertSpaces");
    if (!insert_spaces_val.error()) {
        auto b = insert_spaces_val.get_bool();
        if (!b.error()) {
            fmt_opts.use_tabs = !b.value();
        }
    }

    // Format the file
    Formatter formatter(fmt_opts);
    std::string formatted = formatter.format(*doc.ast);

    // If the formatted output is the same as the original, return empty edits
    if (formatted == doc.content) {
        reply(msg, "[]");
        return;
    }

    // Count lines in original content
    int line_count = 0;
    size_t last_line_start = 0;
    for (size_t i = 0; i < doc.content.size(); ++i) {
        char ch = doc.content[i];
        if (ch == '\n') {
            ++line_count;
            last_line_start = i + 1;
        }
    }
    uint32_t last_line_length =
        lsp_character_for_byte_offset(doc.content, last_line_start, doc.content.size());

    // Return a single TextEdit that replaces the entire document
    std::ostringstream os;
    os << R"([{"range":{"start":{"line":0,"character":0},"end":{"line":)" << line_count
       << R"(,"character":)" << last_line_length << R"(}},"newText":)" << json_escape(formatted)
       << "}]";
    reply(msg, os.str());
}

void FluxLanguageServer::reply(const JsonRpcMessage& msg, const std::string& result_json) {
    if (!msg.id) {
        return; // Can't reply to notifications
    }
    auto response = make_response(*msg.id, result_json);
    transport_.write_message(response);
}

void FluxLanguageServer::reply_error(const JsonRpcMessage& msg,
                                     ErrorCode code,
                                     const std::string& message) {
    if (!msg.id) {
        return;
    }
    auto response = make_error_response(*msg.id, {.code = code, .message = message});
    transport_.write_message(response);
}

void FluxLanguageServer::notify(const std::string& method, const std::string& params_json) {
    auto notification = make_notification(method, params_json);
    transport_.write_message(notification);
}

} // namespace pl::flux::lsp
