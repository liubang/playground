//=====================================================================
//
// tuple_iteration_apply_test.cpp -
//
// Created by liubang on 2023/05/21 23:23
// Last Modified: 2023/05/21 23:23
//
//=====================================================================
#include "cpp/meta/tuple_iteration_apply.h"

#include <catch2/catch_test_macros.hpp>
#include <tuple>
#include <utility>

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
