// Copyright (c) 2024 The Authors. All rights reserved.
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

#include "scanner.h"
#include <iostream>
#include <memory>

int main(int argc, char* argv[]) {

    std::string sql =
        R"(SELECT "level description"::field, "location"::tag, "water_level"::field FROM "h2o_feet" GROUP BY time(600000us) ORDER BY time ASC)";

    std::unique_ptr<pl::Scanner> scanner = std::make_unique<pl::Scanner>(sql.data(), sql.size());

    for (;;) {
        auto tok = scanner->scan();
        if (tok->tok == pl::TokenType::Eof) {
            break;
        }
        std::cout << (*tok) << "\n";
    }

    return 0;
}
