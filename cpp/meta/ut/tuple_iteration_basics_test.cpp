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

#include "cpp/meta/tuple_iteration_basics.h"
#include <catch2/catch_test_macros.hpp>
#include <tuple>
#include <utility>

TEST_CASE("meta", "[PrintTupleManual]") {
    std::tuple tp{10, 20, "hello"};
    pl::PrintTupleManual<decltype(tp), 0, 1, 2>(tp);
    std::cout << std::endl;
};

TEST_CASE("meta", "[PrintTupleManualEx]") {
    std::tuple tp{10, 20, "hello"};
    pl::PrintTupleManualEx(tp, std::index_sequence<0, 1, 2>{});
    std::cout << std::endl;
    pl::PrintTupleManualEx(tp, std::make_index_sequence<std::tuple_size_v<decltype(tp)>>{});
    std::cout << std::endl;
};

TEST_CASE("meta", "[PrintTupleAutoGetSize]") {
    std::tuple tp{10, 20, "hello"};
    pl::PrintTupleAutoGetSize(tp);
    std::cout << std::endl;
};

TEST_CASE("meta", "[PrintTupleFinal]") {
    std::tuple tp{10, 20, "hello"};
    pl::PrintTupleFinal(tp);
    std::cout << std::endl;
};

TEST_CASE("meta", "[PrintTupleWithOstream]") {
    std::tuple tp{10, 20, "hello"};
    std::cout << tp << std::endl;
};
