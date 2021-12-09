#include <gtest/gtest.h>

#include <stack>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
class Solution {
 public:
  int evalRPN(const std::vector<std::string>& tokens) {
    std::stack<int> stk;
    const std::unordered_set<std::string> operators = {"+", "-", "*", "/"};
    for (auto& token : tokens) {
      if (operators.count(token) > 0) {
        if (stk.size() < 2) {
          return 0;
        }
        int rhs = stk.top();
        stk.pop();
        int lhs = stk.top();
        stk.pop();
        if (token == "+") {
          stk.push(lhs + rhs);
        } else if (token == "-") {
          stk.push(lhs - rhs);
        } else if (token == "*") {
          stk.push(lhs * rhs);
        } else if (token == "/") {
          stk.push(lhs / rhs);
        }
      } else {
        stk.push(std::stoi(token));
      }
    }
    if (stk.size() != 1) {
      return 0;
    }
    return stk.top();
  }
};
}  // namespace

TEST(Leetcode, evaluate_reverse_polish_notation) {
  Solution s;
  EXPECT_EQ(9, s.evalRPN({"2", "1", "+", "3", "*"}));
  EXPECT_EQ(6, s.evalRPN({"4", "13", "5", "/", "+"}));
  EXPECT_EQ(22, s.evalRPN({"10", "6", "9", "3", "+", "-11", "*", "/", "*", "17",
                           "+", "5", "+"}));
}
