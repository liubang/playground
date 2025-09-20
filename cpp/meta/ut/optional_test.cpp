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

#include "cpp/meta/optional.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("meta", "[optional]") {
    SECTION("case1") {
        pl::Optional<int> opt1 = 1;
        REQUIRE(opt1.has_value());
        REQUIRE(1 == opt1.value());

        pl::Optional<int> opt2 = opt1;
        REQUIRE(opt1.has_value());
        REQUIRE(1 == opt1.value());
        REQUIRE(opt2.has_value());
        REQUIRE(1 == opt2.value());

        pl::Optional<int> opt3 = std::move(opt2);
        REQUIRE(!opt2.has_value());
        REQUIRE(opt3.has_value());
        REQUIRE(1 == opt3.value());

        pl::Optional<int> opt4 = pl::nullopt;
        REQUIRE(!opt4.has_value());
    }

    SECTION("case2") {
        auto opt1 = pl::Optional<std::vector<int>>({1, 2, 3});
        REQUIRE(opt1.has_value());
        REQUIRE(3 == opt1.value().size());

        auto opt2 = pl::Optional<std::vector<int>>(pl::inplace, {1, 2, 3});
        REQUIRE(opt2.has_value());
        REQUIRE(3 == opt2.value().size());
    }
}
