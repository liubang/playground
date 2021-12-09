#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {
class Solution {
 public:
  std::vector<std::string> generateParenthesis(int n) {
    std::vector<std::string> ret;
    std::string str;
    gen(ret, str, 0, 0, n);
    return ret;
  }

 private:
  void gen(std::vector<std::string>& ret, std::string& str, int open, int close,
           int n) {
    if (str.length() == n * 2) {
      ret.push_back(str);
      return;
    }
    if (open < n) {
      str.push_back('(');
      gen(ret, str, open + 1, close, n);
      str.pop_back();
    }
    if (close < open) {
      str.push_back(')');
      gen(ret, str, open, close + 1, n);
      str.pop_back();
    }
  }
};
}  // namespace

TEST(Leetcode, generate_parentheses) {
  Solution s;
  {
    std::vector<std::string> exps = {
        "((()))", "(()())", "(())()", "()(())", "()()()",
    };
    EXPECT_EQ(exps, s.generateParenthesis(3));
  }

  {
    std::vector<std::string> exps = {"()"};
    EXPECT_EQ(exps, s.generateParenthesis(1));
  }
}
