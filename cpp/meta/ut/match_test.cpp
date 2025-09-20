// Copyright (c) 2025 The Authors. All rights reserved.
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

#include "cpp/meta/match.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>

TEST_CASE("meta", "[match]") {
    std::variant<int, std::string> value[] = {10, "hello"};
    for (auto x : value)
        x >> pl::match{
                 [](int x) {
                     ::printf("x(int) = %d\n", x);
                 },
                 [](const std::string& x) {
                     ::printf("x(string) = %s\n", x.c_str());
                 },
             };
}
