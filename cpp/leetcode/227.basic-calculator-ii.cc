#include <gtest/gtest.h>

#include <numeric>
#include <string>
#include <vector>

namespace {
class Solution {
 public:
  int calculate(const std::string& s) {
    int len = s.length(), num = 0;
    char sign = '+';
    std::vector<int> vec;
    for (int i = 0; i < len; ++i) {
      if (std::isdigit(s[i])) {
        num = num * 10 + (s[i] - '0');
      }
      if ((!std::isdigit(s[i]) && s[i] != ' ') || i == len - 1) {
        switch (sign) {
          case '+':
            vec.push_back(num);
            break;
          case '-':
            vec.push_back(-num);
            break;
          case '*':
            vec.back() *= num;
            break;
          case '/':
            vec.back() /= num;
            break;
        }
        num = 0;
        sign = s[i];
      }
    }
    return std::accumulate(vec.begin(), vec.end(), 0);
  }
};
}  // namespace

TEST(Leetcode, basic_calculator_ii) {
  Solution s;
  EXPECT_EQ(7, s.calculate("3+2*2"));
  EXPECT_EQ(1, s.calculate("3 / 2 "));
  EXPECT_EQ(5, s.calculate(" 3+5 / 2 "));
}
