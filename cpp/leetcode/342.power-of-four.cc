#include <gtest/gtest.h>

namespace {
class Solution {
 public:
  bool isPowerOfFour(int n) {
    if (n <= 0) return false;
    int tmp = n, one = 0, zero = 0;
    while (tmp > 0) {
      if (one >= 1) return false;
      if ((tmp & 1) == 0) {
        zero++;
      } else {
        one++;
      }
      tmp >>= 1;
    }
    return (zero & 1) == 0;
  }
};
}  // namespace

TEST(Leetcode, power_of_four) {
  Solution s;
  EXPECT_FALSE(s.isPowerOfFour(0));
  EXPECT_TRUE(s.isPowerOfFour(1));
  EXPECT_FALSE(s.isPowerOfFour(2));
  EXPECT_TRUE(s.isPowerOfFour(4));
  EXPECT_FALSE(s.isPowerOfFour(8));
  EXPECT_TRUE(s.isPowerOfFour(16));
  EXPECT_FALSE(s.isPowerOfFour(32));
  EXPECT_TRUE(s.isPowerOfFour(64));
}
