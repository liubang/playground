#include <gtest/gtest.h>

#include <climits>
#include <string>

namespace {
class Solution {
 public:
  int myAtoi(const std::string& s) {
    if (s.empty()) {
      return 0;
    }
    int ret = 0, sign = 1;
    int boundary = INT_MAX / 10;
    int len = s.length(), i = 0;
    // 跳过开头的空格
    while (s[i] == ' ' && i < len) {
      i++;
    }
    // 符号位
    if (s[i] == '-') {
      sign = -1;
      i++;
    } else if (s[i] == '+') {
      i++;
    }
    // 数字位
    while (std::isdigit(s[i]) && i < len) {
      if (ret > boundary || (ret == boundary && s[i] > '7')) {
        return sign == 1 ? INT_MAX : INT_MIN;
      }
      ret = ret * 10 + (s[i] - '0');
      i++;
    }
    return ret * sign;
  }
};
}  // namespace

TEST(Leetcode, string_to_integer_atoi) {
  Solution s;
  EXPECT_EQ(42, s.myAtoi("42"));
  EXPECT_EQ(42, s.myAtoi("  42"));
  EXPECT_EQ(-42, s.myAtoi(" -42"));
  EXPECT_EQ(4193, s.myAtoi("4193 with words"));
  EXPECT_EQ(0, s.myAtoi("words and 987"));
  EXPECT_EQ(-2147483648, s.myAtoi("-91283472332"));
}
