#include <gtest/gtest.h>

#include <stack>
#include <string>

namespace {
class Solution {
 public:
  int calculate(const std::string& s) {
    int sign = 1, ret = 0;
    std::stack<int> stk;
    stk.push(1);
    int len = s.length();
    for (int i = 0; i < len; ++i) {
      if (s[i] == ' ') {
        continue;
      } else if (s[i] == '+') {
        sign = stk.top();
      } else if (s[i] == '-') {
        sign = -stk.top();
      } else if (s[i] == '(') {
        stk.push(sign);
      } else if (s[i] == ')') {
        stk.pop();
      } else {
        int num = 0;
        while (s[i] >= '0' && s[i] <= '9' && i < len) {
          num = num * 10 + (s[i] - '0');
          i++;
        }
        ret += sign * num;
        i--;
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, basic_calculator) {
  Solution s;
  {
    std::string input = "1 + 1";
    auto ret = s.calculate(input);
    EXPECT_EQ(2, ret);
  }
  {
    std::string input = " 2-1 + 2 ";
    auto ret = s.calculate(input);
    EXPECT_EQ(3, ret);
  }
  {
    std::string input = "(1+(4+5+2)-3)+(6+8)";
    auto ret = s.calculate(input);
    EXPECT_EQ(23, ret);
  }
}
