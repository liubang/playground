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

#include "cpp/meta/expression.h"
#include <catch2/catch_test_macros.hpp>
#include <iostream>

TEST_CASE("meta", "[expression]") {
    SECTION("case1") {
        auto plus = [](auto x, auto y) {
            return x + y;
        };
        pl::BinaryExpression exp(5, 3.5, plus);
        auto res = exp();
        REQUIRE(8.5 == res);
    }

    SECTION("case2") {
        std::vector<int> x{1, 2, 3}, y{1, 1, 1}, z{2, 5, 3};
        int alpha = 4;
        auto add_scale = [alpha](auto lhs, auto rhs) {
            return lhs + alpha * rhs;
        };
        auto expr = pl::BinaryContainerExpression(x, y, add_scale);

        for (std::size_t i = 0; i < expr.size(); ++i) {
            std::cout << expr[i] << " ";
        }

        std::cout << std::endl;
    }

    SECTION("case3") {
        // x + y + z;
        std::vector<int> x{1, 2, 3}, y{1, 1, 1}, z{2, 5, 3};
        auto plus = [](auto x, auto y) {
            return x + y;
        };
        auto expr =
            pl::BinaryContainerExpression(pl::BinaryContainerExpression(x, y, plus), z, plus);

        for (std::size_t i = 0; i < expr.size(); ++i) {
            std::cout << expr[i] << " ";
        }
        std::cout << std::endl;
    }

    SECTION("case4") {
        std::vector<int> x{1, 2, 3}, y{1, 1, 1}, z{2, 5, 3};
        auto expr = x + y + z;
        for (std::size_t i = 0; i < expr.size(); ++i) {
            std::cout << expr[i] << " ";
        }
        std::cout << std::endl;
    }
}
