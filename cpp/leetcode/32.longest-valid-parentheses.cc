#include <gtest/gtest.h>

#include <stack>
#include <string>

namespace {
class Solution {
 public:
  int longestValidParentheses(const std::string& s) {
    std::stack<int> stack;
    stack.push(-1);
    int ret = 0;
    for (int i = 0; i < s.length(); ++i) {
      if (s[i] == '(') {
        stack.push(i);
      } else {
        stack.pop();
        if (stack.empty()) {
          stack.push(i);
        } else {
          ret = std::max(ret, i - stack.top());
        }
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, longest_valid_parentheses) {
  Solution s;
  EXPECT_EQ(
      s.longestValidParentheses(")(()(()(((())(((((()()))((((()()(()()())())"
                                "())()))()()()())(())()()(((()))))()((()))(("
                                "(())()((()()())((())))(())))())((()())()()("
                                "(()((())))))((()(((((()((()))(()()(())))((("
                                ")))()))())"),
      132);
  EXPECT_EQ(4, s.longestValidParentheses(")()())"));
}
