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

#include "cpp/meta/ut/utils_test.h"
#include "cpp/meta/traits.h"
#include "cpp/meta/utils.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("utils", "[array]") {
    using arr345 = std::array<std::array<std::array<int, 3>, 4>, 5>;
    // 这里的纬度是逆序的
    static_assert(pl::is_same_v<arr345, pl::Array<int, 5, 4, 3>::type>);
    pl::print(1, 2, 3, 4, 5);
};

TEST_CASE("utils", "[map]") {
    using longList = pl::TypeList<char, float, int, double, char>;
    static_assert(std::is_same_v<pl::Map<longList, std::add_pointer>::type,
                                 pl::TypeList<char*, float*, int*, double*, char*>>);
};

TEST_CASE("utils", "[filter]") {
    using longList = pl::TypeList<char, float, int, double, char>;

    static_assert(
        std::is_same_v<pl::Filter<longList, pl::sizeLess4>::type, pl::TypeList<char, char>>);
};

TEST_CASE("utils", "[fold]") {
    using longList = pl::TypeList<char, float, int, double, char>;
    static_assert(
        pl::Fold<longList, std::integral_constant<size_t, 0>, pl::TypeSizeAcc>::type::value == 18);
};

TEST_CASE("utils", "[concat]") {
    static_assert(std::is_same_v<pl::Concat_t<pl::TypeList<int, double>, pl::TypeList<char, float>>,
                                 pl::TypeList<int, double, char, float>>);
};

TEST_CASE("utils", "[elem]") {
    using longList = pl::TypeList<char, float, int, double, char>;
    static_assert(pl::Elem<longList, char>::value);
    static_assert(!pl::Elem<longList, std::string>::value);
};

TEST_CASE("utils", "[unique]") {
    using longList = pl::TypeList<char, float, int, double, char>;
    static_assert(
        std::is_same_v<pl::Unique<longList>::type, pl::TypeList<char, float, int, double>::type>);
};

TEST_CASE("utils", "[sums]") { static_assert(pl::sums(1, 2, 3, 4) == 10); };

TEST_CASE("utils", "[no_destructor]") {
    // smoke test
    pl::NoDestructor<pl::DoNotDestruct> instance(12, 10);
    REQUIRE(12 == instance.get()->a);
    REQUIRE(10 == instance.get()->b);
    REQUIRE(12 == instance->a);
    REQUIRE(10 == instance->b);

    // no copyable
    using T = pl::NoDestructor<int>;
    REQUIRE(!(std::is_constructible<T, T>::value));
    REQUIRE(!(std::is_constructible<T, const T>::value));
    REQUIRE(!(std::is_constructible<T, T&>::value));
    REQUIRE(!(std::is_constructible<T, const T&>::value));

    REQUIRE(!(std::is_assignable<T, T>::value));
    REQUIRE(!(std::is_assignable<T, const T>::value));
    REQUIRE(!(std::is_assignable<T, T&>::value));
    REQUIRE(!(std::is_assignable<T, const T&>::value));
}
