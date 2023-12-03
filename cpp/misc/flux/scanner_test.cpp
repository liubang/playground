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

#include <iostream>
#include <memory>

#include "scanner.h"

int main(int argc, char* argv[]) {
    std::string flux = R"(
    import "array"
    import "math"
    import "influxdata/influxdb/sample"

    from(bucket:"telegraf/autogen")
        |> range(start:-1h)
        |> filter(fn:(r) =>
            r._measurement == "cpu" and
            r.cpu == "cpu-total"
        )
        |> aggregateWindow(every: 1m, fn: mean)
    )";

    std::unique_ptr<pl::Scanner> scanner = std::make_unique<pl::Scanner>(flux.data(), flux.size());

    for (;;) {
        auto token = scanner->scan();
        if (token->tok == pl::TokenType::Eof) {
            break;
        }
        std::cout << (*token) << "\n";
    }

    return 0;
}
