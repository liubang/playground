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

#include "cpp/meta/type_list.h"
#include <catch2/catch_test_macros.hpp>
#include <tuple>
#include <type_traits>
#include <variant>

TEST_CASE("meta", "[type_list]") {
    using AList = pl::TypeList<int, char>;
    static_assert(pl::TL<AList>);
    static_assert(AList::size == 2);
    static_assert(std::is_same_v<AList::prepend<double>, pl::TypeList<double, int, char>>);

    static_assert(std::is_same_v<AList::append<double>, pl::TypeList<int, char, double>>);

    static_assert(std::is_same_v<AList::to<std::tuple>, std::tuple<int, char>>);
    static_assert(std::is_same_v<AList::to<std::variant>, std::variant<int, char>>);
}
