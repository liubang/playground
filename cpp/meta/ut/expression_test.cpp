//=====================================================================
//
// expression_test.cpp -
//
// Created by liubang on 2023/05/21 01:02
// Last Modified: 2023/05/21 01:02
//
//=====================================================================
#include "cpp/meta/expression.h"

#include <catch2/catch_test_macros.hpp>
#include <iostream>

TEST_CASE("meta", "[expression]") {
  SECTION("case1") {
    auto plus = [](auto x, auto y) { return x + y; };
    pl::BinaryExpression exp(5, 3.5, plus);
    auto res = exp();
    REQUIRE(8.5 == res);
  }

  SECTION("case2") {
    std::vector<int> x{1, 2, 3}, y{1, 1, 1}, z{2, 5, 3};
    int alpha = 4;
    auto add_scale = [alpha](auto lhs, auto rhs) { return lhs + alpha * rhs; };
    auto expr = pl::BinaryContainerExpression(x, y, add_scale);

    for (std::size_t i = 0; i < expr.size(); ++i) {
      std::cout << expr[i] << " ";
    }

    std::cout << std::endl;
  }

  SECTION("case3") {
    // x + y + z;
    std::vector<int> x{1, 2, 3}, y{1, 1, 1}, z{2, 5, 3};
    auto plus = [](auto x, auto y) { return x + y; };
    auto expr = pl::BinaryContainerExpression(
        pl::BinaryContainerExpression(x, y, plus), z, plus);

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
