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

#include "cpp/pl/flux/flux_cli.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "cpp/pl/ascii_table/pretty.h"
#include "cpp/pl/flux/ast_debug.h"
#include "cpp/pl/flux/parser.h"
#include "cpp/pl/flux/runtime_builtin.h"
#include "cpp/pl/flux/runtime_exec.h"
#include <simdjson.h>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace pl {
namespace {

std::string status_message(const absl::Status& status) { return std::string(status.message()); }

std::string parser_error_text(const Parser& parser) {
    std::ostringstream out;
    out << "parser errors:\n";
    for (const auto& error : parser.errors()) {
        out << "  - " << error << '\n';
    }
    return out.str();
}

std::vector<std::string> collect_table_columns(const TableValue& table) {
    std::vector<std::string> columns;
    std::unordered_set<std::string> seen;
    for (const auto& row : table.rows) {
        if (row == nullptr) {
            continue;
        }
        for (const auto& [name, value] : row->properties) {
            (void)value;
            if (seen.insert(name).second) {
                columns.push_back(name);
            }
        }
    }
    return columns;
}

bool table_has_column(const TableValue& table, std::string_view name) {
    for (const auto& row : table.rows) {
        if (row == nullptr) {
            continue;
        }
        if (row->lookup(std::string(name)) != nullptr) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> visible_table_columns(const TableValue& table) {
    auto columns = collect_table_columns(table);
    columns.erase(std::remove(columns.begin(), columns.end(), "_group"), columns.end());
    return columns;
}

std::string scalar_cell_text(const Value& value);

std::unordered_set<std::string> collect_group_columns(const TableValue& table) {
    std::unordered_set<std::string> group_columns;
    for (const auto& row : table.rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* group_value = row->lookup("_group");
        if (group_value == nullptr || group_value->type() != Value::Type::Object) {
            continue;
        }
        for (const auto& [name, value] : group_value->as_object().properties) {
            (void)value;
            group_columns.insert(name);
        }
    }
    return group_columns;
}

std::optional<std::string> shared_column_value(const TableValue& table, const std::string& column) {
    std::optional<std::string> shared;
    for (const auto& row : table.rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* value = row->lookup(column);
        if (value == nullptr) {
            continue;
        }
        const std::string current = scalar_cell_text(*value);
        if (!shared.has_value()) {
            shared = current;
            continue;
        }
        if (*shared != current) {
            return std::nullopt;
        }
    }
    return shared;
}

std::string csv_escape(const std::string& value) {
    bool needs_quotes = false;
    for (char ch : value) {
        if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return value;
    }
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

std::string scalar_cell_text(const Value& value) {
    switch (value.type()) {
        case Value::Type::Null:
            return "";
        case Value::Type::Bool:
            return value.as_bool() ? "true" : "false";
        case Value::Type::Int:
            return std::to_string(value.as_int());
        case Value::Type::UInt:
            return std::to_string(value.as_uint());
        case Value::Type::Float: {
            std::ostringstream out;
            out << std::setprecision(15) << value.as_float();
            return out.str();
        }
        case Value::Type::String:
            return value.as_string();
        case Value::Type::Duration:
            return value.as_duration().literal;
        case Value::Type::Time:
            return value.as_time().literal;
        case Value::Type::Regex:
            return value.as_regex().literal;
        case Value::Type::Array:
        case Value::Type::Object:
        case Value::Type::Table:
        case Value::Type::Function:
            return value.string();
    }
}

using JsonBuilder = simdjson::builder::string_builder;

void append_json_field_name(JsonBuilder& builder, std::string_view name, bool& first_field) {
    if (!first_field) {
        builder.append_comma();
    }
    builder.escape_and_append_with_quotes(name);
    builder.append_colon();
    first_field = false;
}

void append_json_value(JsonBuilder& builder, const Value& value);

void append_json_object(JsonBuilder& builder, const ObjectValue& object) {
    builder.start_object();
    bool first_field = true;
    for (const auto& [name, value] : object.properties) {
        append_json_field_name(builder, name, first_field);
        append_json_value(builder, value);
    }
    builder.end_object();
}

void append_json_table(JsonBuilder& builder, const TableValue& table) {
    builder.start_object();
    bool first_field = true;

    append_json_field_name(builder, "type", first_field);
    builder.append(std::string_view("table"));

    append_json_field_name(builder, "bucket", first_field);
    builder.append(table.bucket);

    if (table.range_start.has_value()) {
        append_json_field_name(builder, "rangeStart", first_field);
        builder.append(*table.range_start);
    }
    if (table.range_stop.has_value()) {
        append_json_field_name(builder, "rangeStop", first_field);
        builder.append(*table.range_stop);
    }
    if (table.result_name.has_value()) {
        append_json_field_name(builder, "resultName", first_field);
        builder.append(*table.result_name);
    }

    append_json_field_name(builder, "rows", first_field);
    builder.start_array();
    bool first_row = true;
    for (const auto& row : table.rows) {
        if (!first_row) {
            builder.append_comma();
        }
        if (row == nullptr) {
            builder.append_null();
        } else {
            append_json_object(builder, *row);
        }
        first_row = false;
    }
    builder.end_array();
    builder.end_object();
}

void append_json_value(JsonBuilder& builder, const Value& value) {
    switch (value.type()) {
        case Value::Type::Null:
            builder.append_null();
            return;
        case Value::Type::Bool:
            builder.append(value.as_bool());
            return;
        case Value::Type::Int:
            builder.append(value.as_int());
            return;
        case Value::Type::UInt:
            builder.append(value.as_uint());
            return;
        case Value::Type::Float:
            builder.append(value.as_float());
            return;
        case Value::Type::String:
            builder.append(value.as_string());
            return;
        case Value::Type::Duration:
            builder.append(value.as_duration().literal);
            return;
        case Value::Type::Time:
            builder.append(value.as_time().literal);
            return;
        case Value::Type::Regex:
            builder.append(value.as_regex().literal);
            return;
        case Value::Type::Array: {
            builder.start_array();
            bool first = true;
            for (const auto& element : value.as_array().elements) {
                if (!first) {
                    builder.append_comma();
                }
                append_json_value(builder, element);
                first = false;
            }
            builder.end_array();
            return;
        }
        case Value::Type::Object:
            append_json_object(builder, value.as_object());
            return;
        case Value::Type::Table:
            append_json_table(builder, value.as_table());
            return;
        case Value::Type::Function:
            builder.append(value.string());
            return;
    }
}

std::string flux_datatype_name(const Value& value) {
    switch (value.type()) {
        case Value::Type::Null:
            return "string";
        case Value::Type::Bool:
            return "boolean";
        case Value::Type::Int:
            return "long";
        case Value::Type::UInt:
            return "unsignedLong";
        case Value::Type::Float:
            return "double";
        case Value::Type::String:
            return "string";
        case Value::Type::Duration:
            return "duration";
        case Value::Type::Time:
            return "dateTime:RFC3339";
        case Value::Type::Regex:
            return "string";
        case Value::Type::Array:
        case Value::Type::Object:
        case Value::Type::Table:
        case Value::Type::Function:
            return "string";
    }
}

std::vector<std::string> result_columns(const NamedResult& result) {
    if (result.value.type() == Value::Type::Table) {
        return visible_table_columns(result.value.as_table());
    }
    return {"_value"};
}

std::string column_datatype(const NamedResult& result, const std::string& column) {
    if (result.value.type() == Value::Type::Table) {
        const auto& table = result.value.as_table();
        for (const auto& row : table.rows) {
            if (row == nullptr) {
                continue;
            }
            if (const Value* value = row->lookup(column); value != nullptr && !value->is_null()) {
                return flux_datatype_name(*value);
            }
        }
        return "string";
    }
    return flux_datatype_name(result.value);
}

void append_csv_row(const std::vector<std::string>& cells, std::ostringstream& out) {
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        out << csv_escape(cells[i]);
    }
    out << '\n';
}

void append_annotated_csv_result(const NamedResult& result,
                                 size_t table_index,
                                 std::ostringstream& out) {
    if (result.value.type() == Value::Type::Table) {
        const auto& table = result.value.as_table();
        const auto columns = result_columns(result);
        const bool has_result_column = table_has_column(table, "result");
        const bool has_table_column = table_has_column(table, "table");
        const auto group_columns = collect_group_columns(table);

        std::vector<std::string> datatype_row = {"#datatype"};
        std::vector<std::string> group_row = {"#group"};
        std::vector<std::string> default_row = {"#default"};
        std::vector<std::string> header_row = {""};

        datatype_row.emplace_back("string");
        group_row.emplace_back("false");
        default_row.push_back(has_result_column ? shared_column_value(table, "result").value_or("")
                                                : result.name);
        header_row.emplace_back("result");

        datatype_row.emplace_back("long");
        group_row.emplace_back("false");
        default_row.push_back(has_table_column ? shared_column_value(table, "table").value_or("")
                                               : "");
        header_row.emplace_back("table");

        for (const auto& column : columns) {
            if (column == "result" || column == "table") {
                continue;
            }
            datatype_row.push_back(column_datatype(result, column));
            group_row.emplace_back(group_columns.count(column) != 0 ? "true" : "false");
            default_row.emplace_back("");
            header_row.push_back(column);
        }

        append_csv_row(datatype_row, out);
        append_csv_row(group_row, out);
        append_csv_row(default_row, out);
        append_csv_row(header_row, out);

        for (const auto& row : table.rows) {
            std::vector<std::string> cells = {
                "",
                has_result_column && row != nullptr && row->lookup("result") != nullptr
                    ? scalar_cell_text(*row->lookup("result"))
                    : result.name,
                has_table_column && row != nullptr && row->lookup("table") != nullptr
                    ? scalar_cell_text(*row->lookup("table"))
                    : std::to_string(table_index),
            };
            for (const auto& column : columns) {
                if (column == "result" || column == "table") {
                    continue;
                }
                std::string cell;
                if (row != nullptr) {
                    if (const Value* value = row->lookup(column); value != nullptr) {
                        cell = scalar_cell_text(*value);
                    }
                }
                cells.push_back(cell);
            }
            append_csv_row(cells, out);
        }
        return;
    }

    const auto columns = result_columns(result);
    std::vector<std::string> datatype_row = {"#datatype", "string", "long"};
    std::vector<std::string> group_row = {"#group", "false", "false"};
    std::vector<std::string> default_row = {"#default", result.name, ""};
    std::vector<std::string> header_row = {"", "result", "table"};
    for (const auto& column : columns) {
        datatype_row.push_back(column_datatype(result, column));
        group_row.emplace_back("false");
        default_row.emplace_back("");
        header_row.push_back(column);
    }

    append_csv_row(datatype_row, out);
    append_csv_row(group_row, out);
    append_csv_row(default_row, out);
    append_csv_row(header_row, out);

    std::vector<std::string> cells = {"", result.name, std::to_string(table_index),
                                      scalar_cell_text(result.value)};
    append_csv_row(cells, out);
}

void append_annotated_csv_output(const FileExecutionResult& result, std::ostringstream& out) {
    if (result.results.empty()) {
        return;
    }
    size_t table_index = 0;
    bool first = true;
    for (const auto& named : result.results) {
        if (named.value.is_null()) {
            continue;
        }
        if (!first) {
            out << '\n';
        }
        append_annotated_csv_result(named, table_index++, out);
        first = false;
    }
}

absl::StatusOr<std::string> build_json_output(const FileExecutionResult& result) {
    JsonBuilder builder;
    builder.start_object();
    bool first_field = true;

    append_json_field_name(builder, "package", first_field);
    if (result.package_name.empty()) {
        builder.append_null();
    } else {
        builder.append(result.package_name);
    }

    append_json_field_name(builder, "imports", first_field);
    builder.start_array();
    for (size_t i = 0; i < result.imports.size(); ++i) {
        if (i > 0) {
            builder.append_comma();
        }
        builder.append(result.imports[i]);
    }
    builder.end_array();

    append_json_field_name(builder, "results", first_field);
    builder.start_array();
    bool first = true;
    for (const auto& named : result.results) {
        if (named.value.is_null()) {
            continue;
        }
        if (!first) {
            builder.append_comma();
        }
        builder.start_object();
        bool first_result_field = true;
        append_json_field_name(builder, "name", first_result_field);
        builder.append(named.name);
        append_json_field_name(builder, "value", first_result_field);
        append_json_value(builder, named.value);
        builder.end_object();
        first = false;
    }
    builder.end_array();

    if (!result.last.value.is_null()) {
        append_json_field_name(builder, "last", first_field);
        append_json_value(builder, result.last.value);
    }
    builder.end_object();

    std::string_view view;
    const auto error = builder.view().get(view);
    if (error != simdjson::SUCCESS) {
        return absl::InternalError(std::string("failed to serialize JSON output: ") +
                                   simdjson::error_message(error));
    }
    return std::string(view) + "\n";
}

absl::StatusOr<FileExecutionResult> filter_result_by_name(const FileExecutionResult& result,
                                                          std::string_view result_name) {
    FileExecutionResult filtered;
    filtered.package_name = result.package_name;
    filtered.imports = result.imports;

    std::vector<std::string> available_results;
    available_results.reserve(result.results.size());
    for (const auto& named : result.results) {
        available_results.push_back(named.name);
        if (named.name != result_name) {
            continue;
        }
        filtered.results.push_back(named);
        filtered.last = ExecutionResult::normal(named.value);
    }

    if (!filtered.results.empty()) {
        return filtered;
    }

    if (available_results.empty()) {
        return absl::NotFoundError(std::string("result `") + std::string(result_name) +
                                   "` was not found; script produced no named results");
    }

    return absl::NotFoundError(std::string("result `") + std::string(result_name) +
                               "` was not found; available results: " +
                               absl::StrJoin(available_results, ", "));
}

void append_result_names_output(const FileExecutionResult& result, std::ostringstream& out) {
    for (const auto& named : result.results) {
        if (named.value.is_null()) {
            continue;
        }
        out << named.name << '\n';
    }
}

void append_scalar_result(const NamedResult& result, bool include_header, std::ostringstream& out) {
    if (result.value.is_null()) {
        return;
    }
    if (include_header) {
        out << "Result: " << result.name << '\n';
    }
    out << result.value.string() << '\n';
}

void append_table_result(const NamedResult& result,
                         bool include_header,
                         const FluxCliOptions& options,
                         std::ostringstream& out) {
    const auto& table = result.value.as_table();
    if (include_header) {
        out << "Result: " << result.name << '\n';
    }
    out << "Table: bucket=" << table.bucket << ", rows=" << table.rows.size();
    if (table.range_start.has_value()) {
        out << ", start=" << *table.range_start;
    }
    if (table.range_stop.has_value()) {
        out << ", stop=" << *table.range_stop;
    }
    out << '\n';

    const auto columns = visible_table_columns(table);
    if (columns.empty()) {
        return;
    }
    pretty::Pretty pretty_table(columns);
    pretty_table.set_show_sep(options.table_borders);
    for (const auto& row : table.rows) {
        std::vector<std::string> cells;
        cells.reserve(columns.size());
        for (size_t i = 0; i < columns.size(); ++i) {
            std::string cell = "null";
            if (row != nullptr) {
                if (const Value* value = row->lookup(columns[i]); value != nullptr) {
                    cell = value->string();
                }
            }
            cells.push_back(std::move(cell));
        }
        pretty_table.add_row(cells);
    }
    out << pretty_table.str();
}

void append_named_result(const NamedResult& result,
                         bool include_header,
                         const FluxCliOptions& options,
                         std::ostringstream& out) {
    if (result.value.is_null()) {
        return;
    }
    if (result.value.type() == Value::Type::Table) {
        append_table_result(result, include_header, options, out);
        return;
    }
    append_scalar_result(result, include_header, out);
}

void append_cli_output(const FileExecutionResult& result,
                       const FluxCliOptions& options,
                       std::ostringstream& out) {
    if (result.results.empty()) {
        if (!result.last.value.is_null()) {
            out << result.last.value.string() << '\n';
        }
        return;
    }

    bool multiple_results = result.results.size() > 1;
    bool has_table = std::any_of(result.results.begin(), result.results.end(),
                                 [](const NamedResult& named_result) {
                                     return named_result.value.type() == Value::Type::Table;
                                 });
    bool include_headers = multiple_results || has_table;

    bool first = true;
    for (const auto& named : result.results) {
        if (named.value.is_null()) {
            continue;
        }
        if (!first && include_headers) {
            out << '\n';
        }
        append_named_result(named, include_headers, options, out);
        first = false;
    }
}

std::string_view trim_ascii(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(start, end - start);
}

bool ends_with_token(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool line_requires_more_input(std::string_view line) {
    const auto trimmed = trim_ascii(line);
    if (trimmed.empty()) {
        return false;
    }
    constexpr std::string_view kSuffixes[] = {
        "|>", "=>", "=", ",", ":", "(", "[", "{", "+", "-", "*", "/", "%",
    };
    for (const auto suffix : kSuffixes) {
        if (ends_with_token(trimmed, suffix)) {
            return true;
        }
    }
    return trimmed == "and" || trimmed == "or";
}

bool source_requires_more_input(std::string_view source) {
    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (char ch : source) {
        if (in_string) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        switch (ch) {
            case '(':
                ++paren_depth;
                break;
            case ')':
                if (paren_depth > 0) {
                    --paren_depth;
                }
                break;
            case '[':
                ++bracket_depth;
                break;
            case ']':
                if (bracket_depth > 0) {
                    --bracket_depth;
                }
                break;
            case '{':
                ++brace_depth;
                break;
            case '}':
                if (brace_depth > 0) {
                    --brace_depth;
                }
                break;
            default:
                break;
        }
    }

    if (in_string || paren_depth > 0 || bracket_depth > 0 || brace_depth > 0) {
        return true;
    }

    size_t line_start = source.find_last_of('\n');
    if (line_start == std::string_view::npos) {
        line_start = 0;
    } else {
        ++line_start;
    }
    return line_requires_more_input(source.substr(line_start));
}

bool is_repl_quit_command(std::string_view line) {
    return line == "quit" || line == ":quit" || line == ".quit" || line == "exit" ||
           line == ":exit" || line == ".exit";
}

bool is_repl_help_command(std::string_view line) {
    return line == "help" || line == ":help" || line == ".help";
}

void append_repl_help(std::ostream& output) {
    output << "Flux REPL commands:\n"
           << "  help, :help, .help  Show this help text.\n"
           << "  quit, :quit, .quit, exit, :exit, .exit Leave the REPL.\n";
}

} // namespace

Environment MakeFluxCliEnvironment(const FluxCliOptions& options) {
    Environment env;
    if (options.install_builtins) {
        BuiltinRegistry::Install(env);
    }
    return env;
}

FluxCliResult ExecuteFluxSource(const std::string& source,
                                const std::string& name,
                                Environment& env,
                                const FluxCliOptions& options) {
    Parser parser(source);
    auto file = parser.parse_file(name);
    if (!file) {
        return FluxCliResult{.exit_code = 2, .output = "", .error = "failed to parse input\n"};
    }
    if (!parser.errors().empty()) {
        return FluxCliResult{.exit_code = 2, .output = "", .error = parser_error_text(parser)};
    }

    auto result_or = StatementExecutor::ExecuteFile(*file, env);
    if (!result_or.ok()) {
        return FluxCliResult{
            .exit_code = 1, .output = "", .error = status_message(result_or.status()) + "\n"};
    }
    if (options.result_name.has_value()) {
        auto filtered_or = filter_result_by_name(*result_or, *options.result_name);
        if (!filtered_or.ok()) {
            return FluxCliResult{
                .exit_code = 1,
                .output = "",
                .error = status_message(filtered_or.status()) + "\n"};
        }
        result_or = *filtered_or;
    }

    std::ostringstream out;
    if (options.list_results) {
        append_result_names_output(*result_or, out);
    } else if (!options.quiet) {
        switch (options.output_format) {
            case FluxOutputFormat::Human:
                append_cli_output(*result_or, options, out);
                break;
            case FluxOutputFormat::Csv:
                append_annotated_csv_output(*result_or, out);
                break;
            case FluxOutputFormat::Json:
                if (auto json_or = build_json_output(*result_or); json_or.ok()) {
                    out << *json_or;
                } else {
                    return FluxCliResult{
                        .exit_code = 1, .output = "", .error = std::string(json_or.status().message()) + "\n"};
                }
                break;
        }
    }
    return FluxCliResult{.exit_code = 0, .output = out.str(), .error = ""};
}

FluxCliResult DumpFluxAstSource(const std::string& source,
                                const std::string& name,
                                const FluxAstOptions& options) {
    Parser parser(source);
    auto file = parser.parse_file(name);
    if (!file) {
        return FluxCliResult{.exit_code = 2, .output = "", .error = "failed to parse input\n"};
    }

    std::string error;
    int exit_code = 0;
    if (!parser.errors().empty()) {
        error = parser_error_text(parser);
        exit_code = 2;
    }

    std::string output = options.json ? dump_ast_json(*file) : dump_ast(*file);
    return FluxCliResult{.exit_code = exit_code, .output = output, .error = error};
}

int RunFluxRepl(std::istream& input,
                std::ostream& output,
                std::ostream& error,
                bool interactive,
                const FluxCliOptions& options) {
    auto env = MakeFluxCliEnvironment(options);
    std::string line;
    std::string source;
    int exit_code = 0;

    if (interactive) {
        output << "Flux REPL. Type :help for commands.\n";
    }
    while (true) {
        if (interactive) {
            output << (source.empty() ? "flux> " : "....> ") << std::flush;
        }
        if (!std::getline(input, line)) {
            break;
        }
        if (source.empty() && is_repl_quit_command(line)) {
            break;
        }
        if (source.empty() && is_repl_help_command(line)) {
            append_repl_help(output);
            continue;
        }
        if (line.empty() && source.empty()) {
            continue;
        }
        if (!source.empty()) {
            source += '\n';
        }
        source += line;
        if (source_requires_more_input(source)) {
            continue;
        }

        auto result = ExecuteFluxSource(source, "<repl>", env, options);
        if (!result.output.empty()) {
            output << result.output;
        }
        if (!result.error.empty()) {
            error << result.error;
        }
        if (result.exit_code != 0) {
            exit_code = result.exit_code;
        }
        source.clear();
    }

    if (!source.empty()) {
        auto result = ExecuteFluxSource(source, "<repl>", env, options);
        if (!result.output.empty()) {
            output << result.output;
        }
        if (!result.error.empty()) {
            error << result.error;
        }
        if (result.exit_code != 0) {
            exit_code = result.exit_code;
        }
    }
    return exit_code;
}

} // namespace pl
