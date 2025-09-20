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

#include "cpp/meta/ut/traits_test.h"

#include "cpp/meta/traits.h"
#include <cassert>
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
struct Foo {
    void Init() {}
};
} // namespace

TEST_CASE("meta", "[traits]") {
    static_assert(pl::is_floating_point<float>::value);
    static_assert(pl::is_floating_point<double>::value);
    static_assert(pl::is_floating_point<long double>::value);
    static_assert(!pl::is_floating_point<int>::value);

    static_assert(pl::is_same<int, int>::value);
    static_assert(pl::is_same<std::string, std::string>::value);
    static_assert(!pl::is_same<int, long>::value);

    static_assert(pl::is_same<int, pl::remove_const_t<const int>>::value);

    static_assert(pl::is_same<int, pl::conditional<true, int, std::string>::type>::value);

    static_assert(pl::is_same<std::string, pl::conditional<false, int, std::string>::type>::value);

    static_assert(pl::is_same_v<int, pl::array_size<int[5]>::value_type>);

    static_assert(pl::array_size<int[5]>::value == 5);

    static_assert(pl::ut::numEq(3, 3));

    assert(pl::ut::numEq(3.1, 3.1));

    assert(pl::ut::numEqNew(3, 3));
    assert(pl::ut::numEqNew(3.1, 3.1));

    assert(!pl::HasTypeMember<int>::value);
    assert(pl::HasTypeMember<std::true_type>::value);

    assert(!pl::HasInit<int>::value);
    assert(pl::HasInit<Foo>::value);
}
