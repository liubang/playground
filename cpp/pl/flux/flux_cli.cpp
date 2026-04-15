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

#include "cpp/pl/flux/ast_debug.h"
#include "cpp/pl/flux/parser.h"
#include "cpp/pl/flux/runtime_builtin.h"
#include "cpp/pl/flux/runtime_exec.h"
#include <iostream>
#include <sstream>

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

void append_result_value(const ExecutionResult& result, std::ostringstream& out) {
    if (!result.value.is_null()) {
        out << result.value.string() << '\n';
    }
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
        return FluxCliResult{2, "", "failed to parse input\n"};
    }
    if (!parser.errors().empty()) {
        return FluxCliResult{2, "", parser_error_text(parser)};
    }

    auto result_or = StatementExecutor::ExecuteFile(*file, env);
    if (!result_or.ok()) {
        return FluxCliResult{1, "", status_message(result_or.status()) + "\n"};
    }

    std::ostringstream out;
    if (!options.quiet) {
        append_result_value(result_or->last, out);
    }
    return FluxCliResult{0, out.str(), ""};
}

FluxCliResult DumpFluxAstSource(const std::string& source,
                                const std::string& name,
                                const FluxAstOptions& options) {
    Parser parser(source);
    auto file = parser.parse_file(name);
    if (!file) {
        return FluxCliResult{2, "", "failed to parse input\n"};
    }

    std::string error;
    int exit_code = 0;
    if (!parser.errors().empty()) {
        error = parser_error_text(parser);
        exit_code = 2;
    }

    std::string output = options.json ? dump_ast_json(*file) : dump_ast(*file);
    return FluxCliResult{exit_code, output, error};
}

int RunFluxRepl(std::istream& input,
                std::ostream& output,
                std::ostream& error,
                bool interactive,
                const FluxCliOptions& options) {
    auto env = MakeFluxCliEnvironment(options);
    std::string line;
    int exit_code = 0;

    if (interactive) {
        output << "Flux REPL. Type :quit or :exit to leave.\n";
    }
    while (true) {
        if (interactive) {
            output << "flux> " << std::flush;
        }
        if (!std::getline(input, line)) {
            break;
        }
        if (line == ":quit" || line == ":exit" || line == ".exit") {
            break;
        }
        if (line.empty()) {
            continue;
        }
        auto result = ExecuteFluxSource(line, "<repl>", env, options);
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
