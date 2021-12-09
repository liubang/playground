#include <gtest/gtest.h>

namespace {
class Solution {
 public:
  int trailingZeroes(int n) {
    int ret = 0;
    while (n >= 5) {
      ret += n / 5;
      n /= 5;
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, factorial_trailing_zeros) {
  Solution s;
  EXPECT_EQ(2, s.trailingZeroes(11));
  EXPECT_EQ(0, s.trailingZeroes(3));
  EXPECT_EQ(33, s.trailingZeroes(135));
}
