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

#include "cpp/pl/flux/ast_debug.h"
#include "cpp/pl/flux/parser.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace {

std::string read_all(std::istream& input) {
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

int main(int argc, char* argv[]) {
    std::string source;
    std::string name = "<stdin>";
    bool json = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            json = true;
            continue;
        }
        name = arg;
    }

    if (name != "<stdin>") {
        std::ifstream file(name);
        if (!file) {
            std::cerr << "failed to open " << name << '\n';
            return 1;
        }
        source = read_all(file);
    } else {
        source = read_all(std::cin);
    }

    pl::Parser parser(source);
    auto ast = parser.parse_file(name);
    if (!ast) {
        std::cerr << "failed to parse input\n";
        return 1;
    }

    if (!parser.errors().empty()) {
        std::cerr << "parser errors:\n";
        for (const auto& error : parser.errors()) {
            std::cerr << "  - " << error << '\n';
        }
    }

    std::cout << (json ? pl::dump_ast_json(*ast) : pl::dump_ast(*ast));
    return parser.errors().empty() ? 0 : 2;
}
