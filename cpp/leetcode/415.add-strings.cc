#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace {
class Solution {
 public:
  std::string addStrings(const std::string& num1, const std::string& num2) {
    int i = num1.size() - 1;
    int j = num2.size() - 1;
    int sum = 0;
    std::string ret;
    while (i >= 0 || j >= 0 || sum > 0) {
      if (i >= 0) {
        sum += num1[i--] - '0';
      }
      if (j >= 0) {
        sum += num2[j--] - '0';
      }
      ret = ret.insert(static_cast<size_t>(0), 1, sum % 10 + '0');
      sum /= 10;
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, add_strings) {
  Solution s;

  EXPECT_EQ("2", s.addStrings("1", "1"));
  EXPECT_EQ("1586", s.addStrings("999", "587"));
  EXPECT_EQ("1587587587587587587587587587587586",
            s.addStrings("999999999999999999999999999999999",
                         "587587587587587587587587587587587"));
}
