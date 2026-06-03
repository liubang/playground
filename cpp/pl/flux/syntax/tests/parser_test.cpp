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
// Created: 2023/11/02 12:20

#include <iostream>
#include <memory>

#include "cpp/pl/flux/syntax/ast_debug.h"
#include "cpp/pl/flux/syntax/parser.h"

int main() {
    std::string flux = R"(
    @edition("2022.1")
    package main
    import "array"
    import "math"
    import sample "influxdata/influxdb/sample"

    from(bucket:"telegraf/autogen")
        |> range(start:-1h)
        |> filter(fn:(r) =>
            r._measurement == "cpu" and
            r.cpu == "cpu-total"
        )
        |> aggregateWindow(every: 1m, fn: mean)
    )";
    // std::string flux = R"(
    // from(bucket:"telegraf/autogen")
    //     |> range(start:-1h)
    // )";

    pl::flux::syntax::Parser parser(flux);
    auto file = parser.parse_file("");

    if (!parser.errors().empty()) {
        std::cout << "parser errors:\n";
        for (const auto& error : parser.errors()) {
            std::cout << "  - " << error << '\n';
        }
        std::cout << '\n';
    }

    std::cout << pl::flux::syntax::dump_ast(*file);

    return 0;
}
