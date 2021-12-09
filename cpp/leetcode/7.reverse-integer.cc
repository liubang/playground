#include <gtest/gtest.h>

#include <climits>

namespace {
class Solution {
 public:
  int reverse(int x) {
    long n = 0;
    while (x != 0) {
      n = n * 10 + x % 10;
      x /= 10;
    }
    return n > INT_MAX || n < INT_MIN ? 0 : n;
  }
};
}  // namespace

TEST(Leetcode, reverse_integer) {
  Solution s;
  EXPECT_EQ(321, s.reverse(123));
  EXPECT_EQ(-321, s.reverse(-123));
  EXPECT_EQ(0, s.reverse(2147483647));
}
