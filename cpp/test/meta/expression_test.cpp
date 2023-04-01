#include "expression.h"
#include <cassert>
#include <iostream>
#include <vector>

int main(int argc, char* argv[]) {
  {
    auto plus = [](auto x, auto y) { return x + y; };
    highkyck::meta::BinaryExpression exp(5, 3.5, plus);
    auto res = exp();
    assert(8.5 == res);
  }

  {
    std::vector<int> x{1, 2, 3}, y{1, 1, 1}, z{2, 5, 3};
    auto plus = [](auto x, auto y) { return x + y; };
    int alpha = 4;

    {
      auto add_scale = [alpha](auto lhs, auto rhs) {
        return lhs + alpha * rhs;
      };
      auto expr = highkyck::meta::BinaryContainerExpression(x, y, add_scale);
      for (std::size_t i = 0; i < expr.size(); ++i) {
        std::cout << expr[i] << " ";
      }
      std::cout << std::endl;
    }

    {
      // x + y + z;
      auto expr = highkyck::meta::BinaryContainerExpression(
          highkyck::meta::BinaryContainerExpression(x, y, plus), z, plus);

      for (std::size_t i = 0; i < expr.size(); ++i) {
        std::cout << expr[i] << " ";
      }
      std::cout << std::endl;
    }

    {
      auto expr = x + y + z;
      for (std::size_t i = 0; i < expr.size(); ++i) {
        std::cout << expr[i] << " ";
      }
      std::cout << std::endl;
    }
  }

  return 0;
}
