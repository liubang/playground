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

#include "cpp/pl/flux/contrib/lsp/formatter.h"
#include "cpp/pl/flux/syntax/parser.h"
#include "simdjson.h"
#include <algorithm>
#include <sstream>

namespace pl::flux::lsp {

namespace {

// Extract a string field from a simdjson dom element at a given JSON pointer path.
std::string get_string_field(const simdjson::dom::element& elem, std::string_view pointer) {
    auto val = elem.at_pointer(pointer);
    if (val.error()) return "";
    auto s = val.get_string();
    if (s.error()) return "";
    return std::string(s.value());
}

int64_t get_int_field(const simdjson::dom::element& elem, std::string_view pointer) {
    auto val = elem.at_pointer(pointer);
    if (val.error()) return 0;
    auto n = val.get_int64();
    if (n.error()) return 0;
    return n.value();
}

// Escape a string for JSON output
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
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
                out += c;
        }
    }
    out += '"';
    return out;
}

// Get the word at a given line/character position in source text.
std::string word_at_position(const std::string& content, int line, int character) {
    // Find the line
    int current_line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < content.size() && current_line < line; ++i) {
        if (content[i] == '\n') {
            ++current_line;
            line_start = i + 1;
        }
    }

    size_t pos = line_start + static_cast<size_t>(character);
    if (pos >= content.size())
        return "";

    // Find word boundaries
    size_t start = pos;
    while (start > line_start && (std::isalnum(content[start - 1]) || content[start - 1] == '_')) {
        --start;
    }
    size_t end = pos;
    while (end < content.size() && (std::isalnum(content[end]) || content[end] == '_')) {
        ++end;
    }

    if (start == end)
        return "";
    return content.substr(start, end - start);
}

// Check if position is after a pipe operator '|>'
bool is_after_pipe(const std::string& content, int line, int character) {
    int current_line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < content.size() && current_line < line; ++i) {
        if (content[i] == '\n') {
            ++current_line;
            line_start = i + 1;
        }
    }

    size_t pos = line_start + static_cast<size_t>(character);
    // Look backwards for |>
    if (pos < 2)
        return false;
    for (size_t i = pos; i >= 2; --i) {
        char c = content[i - 1];
        if (c == ' ' || c == '\t')
            continue;
        if (c == '>' && content[i - 2] == '|')
            return true;
        break;
    }
    return false;
}

// Check if position is after a dot (member access / package qualifier)
std::string prefix_before_dot(const std::string& content, int line, int character) {
    int current_line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < content.size() && current_line < line; ++i) {
        if (content[i] == '\n') {
            ++current_line;
            line_start = i + 1;
        }
    }

    size_t pos = line_start + static_cast<size_t>(character);
    if (pos == 0)
        return "";

    // Find the dot
    size_t dot_pos = pos - 1;
    while (dot_pos > line_start && content[dot_pos] == ' ')
        --dot_pos;
    // Look for the char just typed or the context
    // Actually check backwards from cursor for "word."
    size_t i = pos;
    while (i > line_start && (std::isalnum(content[i - 1]) || content[i - 1] == '_'))
        --i;
    // Check if there's a dot before
    if (i > line_start && content[i - 1] == '.') {
        size_t dot = i - 1;
        size_t word_start = dot;
        while (word_start > line_start &&
               (std::isalnum(content[word_start - 1]) || content[word_start - 1] == '_')) {
            --word_start;
        }
        if (word_start < dot) {
            return content.substr(word_start, dot - word_start);
        }
    }
    return "";
}

// Known universe builtin table transforms (for pipe completion)
const std::vector<std::pair<std::string, std::string>>& universe_table_builtins() {
    static const std::vector<std::pair<std::string, std::string>> builtins = {
        {"range", "range(start:, stop:) - Filter by time range"},
        {"filter", "filter(fn:) - Filter rows by predicate"},
        {"map", "map(fn:) - Transform each row"},
        {"limit", "limit(n:) - Take first n rows"},
        {"tail", "tail(n:) - Take last n rows"},
        {"keep", "keep(columns:) - Keep specified columns"},
        {"drop", "drop(columns:) - Drop specified columns"},
        {"rename", "rename(columns:) - Rename columns"},
        {"duplicate", "duplicate(column:, as:) - Duplicate a column"},
        {"set", "set(key:, value:) - Set column value"},
        {"sort", "sort(columns:, desc:) - Sort by columns"},
        {"group", "group(columns:) - Group by columns"},
        {"pivot", "pivot(rowKey:, columnKey:, valueColumn:) - Pivot table"},
        {"fill", "fill(column:, value:) - Fill null values"},
        {"union", "union(tables:) - Union multiple table streams"},
        {"count", "count(column:) - Count rows"},
        {"sum", "sum() - Sum values"},
        {"mean", "mean() - Calculate mean"},
        {"min", "min() - Find minimum"},
        {"max", "max() - Find maximum"},
        {"spread", "spread(column:) - Max minus min"},
        {"quantile", "quantile(q:, column:) - Calculate quantile"},
        {"median", "median(column:) - Calculate median"},
        {"first", "first() - First row per group"},
        {"last", "last() - Last row per group"},
        {"top", "top(n:, columns:) - Top n rows"},
        {"bottom", "bottom(n:, columns:) - Bottom n rows"},
        {"reduce", "reduce(identity:, fn:) - Fold rows"},
        {"distinct", "distinct(column:) - Unique values"},
        {"window", "window(every:) - Time-based windowing"},
        {"aggregateWindow", "aggregateWindow(every:, fn:) - Window + aggregate"},
        {"elapsed", "elapsed(unit:) - Time between rows"},
        {"difference", "difference(columns:) - Row-to-row difference"},
        {"derivative", "derivative(unit:) - Rate of change"},
        {"join", "join(tables:, on:) - Join tables"},
        {"columns", "columns() - List column names"},
        {"keys", "keys() - List group key columns"},
        {"findColumn", "findColumn(fn:, column:) - Extract column as array"},
        {"findRecord", "findRecord(fn:, idx:) - Extract record"},
        {"explain", "explain() - Show query plan"},
        {"yield", "yield(name:) - Name and output result"},
    };
    return builtins;
}

// Known packages and their exports
const std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>&
known_packages() {
    static const std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>
        pkgs = {
            {"array",
             {{"from", "array.from(rows:) - Create table from array"},
              {"concat", "array.concat(arr:, v:) - Concatenate arrays"},
              {"filter", "array.filter(arr:, fn:) - Filter array"},
              {"map", "array.map(arr:, fn:) - Map array"},
              {"contains", "array.contains(arr:, value:) - Check membership"},
              {"reduce", "array.reduce(arr:, identity:, fn:) - Fold array"},
              {"any", "array.any(arr:, fn:) - Any match"},
              {"all", "array.all(arr:, fn:) - All match"}}},
            {"csv", {{"from", "csv.from(csv:, file:, mode:) - Parse CSV to table"}}},
            {"sqlite", {{"from", "sqlite.from(path:, table:) - Scan SQLite table"}}},
            {"mysql", {{"from", "mysql.from(dsn:, table:) - Scan MySQL table"}}},
            {"date",
             {{"add", "date.add(d:, to:) - Add duration to time"},
              {"sub", "date.sub(d:, from:) - Subtract duration"},
              {"truncate", "date.truncate(t:, unit:) - Truncate time"},
              {"year", "date.year(t:) - Extract year"},
              {"month", "date.month(t:) - Extract month"},
              {"monthDay", "date.monthDay(t:) - Day of month"},
              {"weekDay", "date.weekDay(t:) - Day of week"},
              {"hour", "date.hour(t:) - Extract hour"},
              {"minute", "date.minute(t:) - Extract minute"},
              {"second", "date.second(t:) - Extract second"}}},
            {"dict",
             {{"fromList", "dict.fromList(pairs:) - Create dict"},
              {"get", "dict.get(dict:, key:, default:) - Get value"},
              {"insert", "dict.insert(dict:, key:, value:) - Insert"},
              {"remove", "dict.remove(dict:, key:) - Remove key"}}},
            {"join",
             {{"inner", "join.inner(left:, right:, on:) - Inner join"},
              {"left", "join.left(left:, right:, on:) - Left join"},
              {"right", "join.right(left:, right:, on:) - Right join"},
              {"full", "join.full(left:, right:, on:) - Full join"}}},
            {"json", {{"encode", "json.encode(v:) - Encode to JSON string"}}},
            {"math",
             {{"abs", "math.abs(x:) - Absolute value"},
              {"ceil", "math.ceil(x:) - Ceiling"},
              {"floor", "math.floor(x:) - Floor"},
              {"round", "math.round(x:) - Round"},
              {"sqrt", "math.sqrt(x:) - Square root"},
              {"pow", "math.pow(x:, y:) - Power"}}},
            {"regexp",
             {{"compile", "regexp.compile(v:) - Compile regex"},
              {"findString", "regexp.findString(r:, v:) - Find match"},
              {"matchRegexpString", "regexp.matchRegexpString(r:, v:) - Test match"},
              {"quoteMeta", "regexp.quoteMeta(v:) - Escape metacharacters"}}},
            {"runtime", {{"version", "runtime.version() - Runtime version"}}},
            {"strings",
             {{"containsStr", "strings.containsStr(v:, substr:) - Contains"},
              {"hasPrefix", "strings.hasPrefix(v:, prefix:) - Has prefix"},
              {"hasSuffix", "strings.hasSuffix(v:, suffix:) - Has suffix"},
              {"joinStr", "strings.joinStr(arr:, v:) - Join strings"},
              {"replaceAll", "strings.replaceAll(v:, t:, u:) - Replace all"},
              {"split", "strings.split(v:, t:) - Split string"},
              {"toUpper", "strings.toUpper(v:) - To uppercase"},
              {"toLower", "strings.toLower(v:) - To lowercase"},
              {"trimSpace", "strings.trimSpace(v:) - Trim whitespace"}}},
            {"system", {{"time", "system.time() - Current UTC time"}}},
            {"types",
             {{"isNumeric", "types.isNumeric(v:) - Is numeric"},
              {"isType", "types.isType(v:, type:) - Check type"},
              {"isString", "types.isString(v:) - Is string"},
              {"isDuration", "types.isDuration(v:) - Is duration"},
              {"isBool", "types.isBool(v:) - Is bool"},
              {"isInt", "types.isInt(v:) - Is int"},
              {"isUInt", "types.isUInt(v:) - Is uint"},
              {"isFloat", "types.isFloat(v:) - Is float"},
              {"isTime", "types.isTime(v:) - Is time"},
              {"isRegexp", "types.isRegexp(v:) - Is regexp"}}},
        };
    return pkgs;
}

// Keywords for basic completion
const std::vector<std::string>& flux_keywords() {
    static const std::vector<std::string> kws = {
        "and",     "or",     "not",      "if",     "then", "else",    "import", "package", "option",
        "builtin", "return", "testcase", "exists", "with", "extends", "true",   "false",
    };
    return kws;
}

} // namespace

FluxLanguageServer::FluxLanguageServer(StdioTransport transport, ServerOptions opts)
    : transport_(std::move(transport)), opts_(std::move(opts)) {}

void FluxLanguageServer::run() {
    while (!shutdown_requested_) {
        auto message = transport_.read_message();
        if (!message)
            break; // EOF

        auto parsed = parse_message(*message);
        if (!parsed)
            continue; // Malformed message, skip

        dispatch(*parsed);
    }
}

void FluxLanguageServer::dispatch(const JsonRpcMessage& msg) {
    if (msg.method == "initialize") {
        handle_initialize(msg);
    } else if (msg.method == "initialized") {
        handle_initialized(msg);
    } else if (msg.method == "shutdown") {
        handle_shutdown(msg);
    } else if (msg.method == "exit") {
        handle_exit(msg);
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
    } else {
        // Unknown method: if it's a request (has id), reply with MethodNotFound
        if (msg.id) {
            reply_error(msg, ErrorCode::MethodNotFound, "Method not found: " + msg.method);
        }
    }
}

void FluxLanguageServer::handle_initialize(const JsonRpcMessage& msg) {
    initialized_ = true;

    // Read initializationOptions to override server-level format settings.
    // Supported fields:
    //   initializationOptions.maxLineWidth: int
    //   initializationOptions.indentWidth: int
    //   initializationOptions.useTabs: bool
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
      "change": 1
    },
    "completionProvider": {
      "triggerCharacters": [".", "|"],
      "resolveProvider": false
    },
    "hoverProvider": true,
    "documentFormattingProvider": true
  },
  "serverInfo": {
    "name": "flux-ls",
    "version": "0.1.0"
  }
})";
    reply(msg, result);
}

void FluxLanguageServer::handle_initialized(const JsonRpcMessage& /*msg*/) {
    // Nothing to do, but we could log here
}

void FluxLanguageServer::handle_shutdown(const JsonRpcMessage& msg) {
    shutdown_requested_ = true;
    reply(msg, "null");
}

void FluxLanguageServer::handle_exit(const JsonRpcMessage& /*msg*/) { shutdown_requested_ = true; }

void FluxLanguageServer::handle_did_open(const JsonRpcMessage& msg) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(msg.params);
    if (doc.error()) return;

    auto uri = get_string_field(doc.value(), "/textDocument/uri");
    auto text = get_string_field(doc.value(), "/textDocument/text");

    Document& d = documents_[uri];
    d.uri = uri;
    d.content = text;
    d.version = 0;

    publish_diagnostics(uri);
}

void FluxLanguageServer::handle_did_change(const JsonRpcMessage& msg) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(msg.params);
    if (doc.error()) return;

    auto uri = get_string_field(doc.value(), "/textDocument/uri");

    auto it = documents_.find(uri);
    if (it == documents_.end()) return;

    // Full sync mode (change: 1) - take the last content change
    auto changes = doc["contentChanges"];
    if (changes.error()) return;

    auto arr = changes.get_array();
    if (arr.error()) return;

    std::string new_text;
    for (auto change : arr.value()) {
        auto text_val = change["text"];
        if (!text_val.error()) {
            auto s = text_val.get_string();
            if (!s.error()) {
                new_text = std::string(s.value());
            }
        }
    }

    it->second.content = new_text;
    it->second.version++;

    publish_diagnostics(uri);
}

void FluxLanguageServer::handle_did_close(const JsonRpcMessage& msg) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(msg.params);
    if (doc.error()) return;

    auto uri = get_string_field(doc.value(), "/textDocument/uri");
    documents_.erase(uri);

    // Clear diagnostics
    std::ostringstream os;
    os << R"({"uri":)" << json_escape(uri) << R"(,"diagnostics":[]})";
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

    auto result =
        build_hover_response(it->second, static_cast<int>(line), static_cast<int>(character));
    reply(msg, result);
}

void FluxLanguageServer::publish_diagnostics(const std::string& uri) {
    auto it = documents_.find(uri);
    if (it == documents_.end())
        return;

    const auto& content = it->second.content;

    // Parse the document using parse_file (safe for files without package clause)
    Parser parser(content);
    auto file = parser.parse_file(uri);
    const auto& errors = parser.errors();

    std::ostringstream os;
    os << R"({"uri":)" << json_escape(uri) << R"(,"diagnostics":[)";

    for (size_t i = 0; i < errors.size(); ++i) {
        if (i > 0)
            os << ",";
        // We don't have precise position from parser errors yet,
        // so report at line 0 col 0 with the error message.
        // TODO: extract line/col from error messages when parser supports it.
        os << R"({"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}})"
           << R"(,"severity":1,"source":"flux-ls","message":)" << json_escape(errors[i]) << "}";
    }

    os << "]}";
    notify("textDocument/publishDiagnostics", os.str());
}

std::string FluxLanguageServer::build_completion_response(const Document& doc,
                                                          int line,
                                                          int character) {
    std::ostringstream os;
    os << R"({"isIncomplete":false,"items":[)";

    bool first = true;
    auto add_item = [&](const std::string& label, const std::string& detail, int kind) {
        if (!first)
            os << ",";
        first = false;
        os << R"({"label":)" << json_escape(label) << R"(,"kind":)" << kind << R"(,"detail":)"
           << json_escape(detail) << "}";
    };

    // Check context
    auto pkg_prefix = prefix_before_dot(doc.content, line, character);

    if (!pkg_prefix.empty()) {
        // Package member completion
        auto& pkgs = known_packages();
        auto pkg_it = pkgs.find(pkg_prefix);
        if (pkg_it != pkgs.end()) {
            for (const auto& [name, detail] : pkg_it->second) {
                add_item(name, detail, 3 /* Function */);
            }
        }
    } else if (is_after_pipe(doc.content, line, character)) {
        // After |> : suggest table transforms
        for (const auto& [name, detail] : universe_table_builtins()) {
            add_item(name, detail, 3 /* Function */);
        }
    } else {
        // General completion: keywords + universe builtins + known packages
        for (const auto& kw : flux_keywords()) {
            add_item(kw, "keyword", 14 /* Keyword */);
        }
        for (const auto& [name, detail] : universe_table_builtins()) {
            add_item(name, detail, 3 /* Function */);
        }
        // Package names
        for (const auto& [pkg_name, _] : known_packages()) {
            add_item(pkg_name, "package", 9 /* Module */);
        }
    }

    os << "]}";
    return os.str();
}

std::string FluxLanguageServer::build_hover_response(const Document& doc, int line, int character) {
    auto word = word_at_position(doc.content, line, character);
    if (word.empty())
        return "null";

    // Check if it's a builtin
    for (const auto& [name, detail] : universe_table_builtins()) {
        if (name == word) {
            std::ostringstream os;
            os << R"({"contents":{"kind":"markdown","value":)"
               << json_escape("**" + name + "**\n\n" + detail) << "}}";
            return os.str();
        }
    }

    // Check packages
    auto pkg_prefix = prefix_before_dot(doc.content, line, character);
    if (!pkg_prefix.empty()) {
        auto& pkgs = known_packages();
        auto pkg_it = pkgs.find(pkg_prefix);
        if (pkg_it != pkgs.end()) {
            for (const auto& [name, detail] : pkg_it->second) {
                if (name == word) {
                    std::ostringstream os;
                    os << R"({"contents":{"kind":"markdown","value":)"
                       << json_escape("**" + pkg_prefix + "." + name + "**\n\n" + detail) << "}}";
                    return os.str();
                }
            }
        }
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

void FluxLanguageServer::handle_formatting(const JsonRpcMessage& msg) {
    simdjson::dom::parser json_parser;
    auto doc = json_parser.parse(msg.params);
    if (doc.error()) {
        reply(msg, "[]");
        return;
    }

    auto uri = get_string_field(doc.value(), "/textDocument/uri");

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        reply(msg, "[]");
        return;
    }

    const auto& content = it->second.content;

    // Parse the document
    Parser parser(content);
    auto file = parser.parse_file(uri);
    const auto& errors = parser.errors();

    // Only format if there are no parse errors
    if (!errors.empty()) {
        reply(msg, "[]");
        return;
    }

    // Start with server-level format options, then override with per-request options.
    Formatter::Options fmt_opts = opts_.format;
    auto tab_size = get_int_field(doc.value(), "/options/tabSize");
    if (tab_size > 0) {
        fmt_opts.indent_width = static_cast<int>(tab_size);
    }
    auto insert_spaces_val = doc.value().at_pointer("/options/insertSpaces");
    if (!insert_spaces_val.error()) {
        auto b = insert_spaces_val.get_bool();
        if (!b.error()) {
            fmt_opts.use_tabs = !b.value();
        }
    }

    // Format the file
    Formatter formatter(fmt_opts);
    std::string formatted = formatter.format(*file);

    // If the formatted output is the same as the original, return empty edits
    if (formatted == content) {
        reply(msg, "[]");
        return;
    }

    // Count lines in original content
    int line_count = 0;
    int last_line_length = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            ++line_count;
            last_line_length = 0;
        } else {
            ++last_line_length;
        }
    }

    // Return a single TextEdit that replaces the entire document
    std::ostringstream os;
    os << R"([{"range":{"start":{"line":0,"character":0},"end":{"line":)" << line_count
       << R"(,"character":)" << last_line_length << R"(}},"newText":)" << json_escape(formatted)
       << "}]";
    reply(msg, os.str());
}

void FluxLanguageServer::reply(const JsonRpcMessage& msg, const std::string& result_json) {
    if (!msg.id)
        return; // Can't reply to notifications
    auto response = make_response(*msg.id, result_json);
    transport_.write_message(response);
}

void FluxLanguageServer::reply_error(const JsonRpcMessage& msg,
                                     ErrorCode code,
                                     const std::string& message) {
    if (!msg.id)
        return;
    auto response = make_error_response(*msg.id, {code, message});
    transport_.write_message(response);
}

void FluxLanguageServer::notify(const std::string& method, const std::string& params_json) {
    auto notification = make_notification(method, params_json);
    transport_.write_message(notification);
}

} // namespace pl::flux::lsp
