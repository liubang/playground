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

#include "cpp/meta/tuple_iteration_apply.h"
#include <catch2/catch_test_macros.hpp>
#include <tuple>

TEST_CASE("meta", "[PrintTupleApplyFn]") {
    std::tuple tp{10, 20, "hello"};
    pl::PrintTupleApplyFn(tp);
    std::cout << std::endl;
    REQUIRE(true);
};

TEST_CASE("meta", "[PrintTupleApply]") {
    std::tuple tp{10, 20, 3.14, 42, "hello"};
    pl::PrintTupleApply(tp);
    std::cout << std::endl;
    REQUIRE(true);
};
