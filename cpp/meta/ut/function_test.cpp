//=====================================================================
//
// function_test.cpp -
//
// Created by liubang on 2023/10/10 00:18
// Last Modified: 2023/10/10 00:18
//
//=====================================================================

#include "cpp/meta/function.h"

#include <catch2/catch_test_macros.hpp>

static int foo(pl::Function<int(int)> const& func) { return func(1) + func(2); }

TEST_CASE("meta", "[function]") {
    int i = foo([](int i) {
        return i * 3;
    });

    REQUIRE(i == 9);
}
