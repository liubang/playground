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
// Created: 2026/09/13 15:52

#include <iostream>
#include <string>

#include "cpp/pl/prism/syntax/parser.h"

// Classifies SQL text read from stdin, one entry per line:
//   S<TAB>sql  parses as a statement
//   E<TAB>sql  parses as an expression
//   F<TAB>sql  parses as neither (unsupported or invalid)
// Used to build and curate the golden corpus from Trino's TestSqlParser.
int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        {
            pl::prism::syntax::Parser parser(line);
            if (parser.parse_statement().ok()) {
                std::cout << "S\t" << line << '\n';
                continue;
            }
        }
        {
            pl::prism::syntax::Parser parser(line);
            if (parser.parse_expression().ok()) {
                std::cout << "E\t" << line << '\n';
                continue;
            }
        }
        std::cout << "F\t" << line << '\n';
    }
    return 0;
}
