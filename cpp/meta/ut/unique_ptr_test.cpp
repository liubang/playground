//=====================================================================
//
// unique_ptr_test.cpp -
//
// Created by liubang on 2023/10/15 01:07
// Last Modified: 2023/10/15 01:07
//
//=====================================================================
#include "cpp/meta/unique_ptr.h"
#include <catch2/catch_test_macros.hpp>
#include <string>

TEST_CASE("meta", "[unique_ptr]") {
    auto str_ptr = pl::make_unique<std::string>("hello");
    str_ptr->append(" world");

    REQUIRE(*(str_ptr.get()) == "hello world");

    auto str_ptr2 = std::move(str_ptr);
    REQUIRE(*(str_ptr2.get()) == "hello world");
    REQUIRE(str_ptr.get() == nullptr);
}
