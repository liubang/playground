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
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

std::string read_all(std::istream& input) {
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void print_usage(std::ostream& out) {
    out << "usage: flux [--repl] [--quiet] [--no-prelude] [file.flux]\n"
        << "       flux -e 'source'\n"
        << "       flux ast [--json] [file.flux]\n"
        << "       flux ast -e 'source'\n\n"
        << "Without a file or -e, flux starts a small REPL.\n";
}

int run_ast_command(int argc, char* argv[]) {
    pl::FluxAstOptions options;
    std::optional<std::string> eval_source;
    std::optional<std::string> file_name;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "--json") {
            options.json = true;
            continue;
        }
        if (arg == "--eval" || arg == "-e") {
            if (i + 1 >= argc) {
                std::cerr << arg << " requires source text\n";
                return 1;
            }
            eval_source = argv[++i];
            continue;
        }
        if (file_name.has_value()) {
            std::cerr << "unexpected extra argument: " << arg << '\n';
            print_usage(std::cerr);
            return 1;
        }
        file_name = arg;
    }

    if (eval_source.has_value() && file_name.has_value()) {
        std::cerr << "-e cannot be combined with a file\n";
        return 1;
    }

    std::string source;
    std::string name = "<stdin>";
    if (eval_source.has_value()) {
        source = *eval_source;
        name = "<eval>";
    } else if (file_name.has_value()) {
        name = *file_name;
        std::ifstream file(name);
        if (!file) {
            std::cerr << "failed to open " << name << '\n';
            return 1;
        }
        source = read_all(file);
    } else {
        source = read_all(std::cin);
    }

    auto result = pl::DumpFluxAstSource(source, name, options);
    std::cout << result.output;
    std::cerr << result.error;
    return result.exit_code;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "ast") {
        return run_ast_command(argc, argv);
    }

    pl::FluxCliOptions options;
    bool repl = false;
    std::optional<std::string> eval_source;
    std::optional<std::string> file_name;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "--repl" || arg == "-i") {
            repl = true;
            continue;
        }
        if (arg == "--quiet" || arg == "-q") {
            options.quiet = true;
            continue;
        }
        if (arg == "--no-prelude") {
            options.install_builtins = false;
            continue;
        }
        if (arg == "--eval" || arg == "-e") {
            if (i + 1 >= argc) {
                std::cerr << arg << " requires source text\n";
                return 1;
            }
            eval_source = argv[++i];
            continue;
        }
        if (file_name.has_value()) {
            std::cerr << "unexpected extra argument: " << arg << '\n';
            print_usage(std::cerr);
            return 1;
        }
        file_name = arg;
    }

    if (eval_source.has_value() && (file_name.has_value() || repl)) {
        std::cerr << "-e cannot be combined with a file or --repl\n";
        return 1;
    }
    if (repl && file_name.has_value()) {
        std::cerr << "--repl cannot be combined with a file\n";
        return 1;
    }

    if (repl || (!file_name.has_value() && !eval_source.has_value())) {
        return pl::RunFluxRepl(std::cin, std::cout, std::cerr, isatty(STDIN_FILENO), options);
    }

    std::string source;
    std::string name = "<eval>";
    if (eval_source.has_value()) {
        source = *eval_source;
    } else {
        name = *file_name;
        std::ifstream file(name);
        if (!file) {
            std::cerr << "failed to open " << name << '\n';
            return 1;
        }
        source = read_all(file);
    }

    auto env = pl::MakeFluxCliEnvironment(options);
    auto result = pl::ExecuteFluxSource(source, name, env, options);
    std::cout << result.output;
    std::cerr << result.error;
    return result.exit_code;
}
