//=====================================================================
//
// expression_test.cpp -
//
// Created by liubang on 2023/05/21 01:02
// Last Modified: 2023/05/21 01:02
//
//=====================================================================
#include "cpp/meta/expression.h"

#include <gtest/gtest.h>
#include <iostream>

TEST(meta, expression) {
  {
    auto plus = [](auto x, auto y) { return x + y; };
    pl::meta::BinaryExpression exp(5, 3.5, plus);
    auto res = exp();
    EXPECT_EQ(8.5, res);
  }

  {
    std::vector<int> x{1, 2, 3}, y{1, 1, 1}, z{2, 5, 3};
    int alpha = 4;
    auto add_scale = [alpha](auto lhs, auto rhs) { return lhs + alpha * rhs; };
    auto expr =
        pl::meta::BinaryContainerExpression(x, y, add_scale);

    for (std::size_t i = 0; i < expr.size(); ++i) {
      std::cout << expr[i] << " ";
    }

    std::cout << std::endl;
  }

  {
    // x + y + z;
    std::vector<int> x{1, 2, 3}, y{1, 1, 1}, z{2, 5, 3};
    auto plus = [](auto x, auto y) { return x + y; };
    auto expr = pl::meta::BinaryContainerExpression(
        pl::meta::BinaryContainerExpression(x, y, plus), z, plus);

    for (std::size_t i = 0; i < expr.size(); ++i) {
      std::cout << expr[i] << " ";
    }
    std::cout << std::endl;
  }

  {
    std::vector<int> x{1, 2, 3}, y{1, 1, 1}, z{2, 5, 3};
    auto expr = x + y + z;
    for (std::size_t i = 0; i < expr.size(); ++i) {
      std::cout << expr[i] << " ";
    }
    std::cout << std::endl;
  }
}
