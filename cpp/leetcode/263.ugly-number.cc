#include <gtest/gtest.h>

#include <cmath>

namespace {
class Solution {
 public:
  bool isUgly(int n) {
    if (n <= 0) {
      return false;
    }
    int s = std::sqrt(n);
    for (int i = 2; i <= s; ++i) {
      while (n % i == 0) {
        if (i != 2 && i != 3 && i != 5) {
          return false;
        }
        n /= i;
      }
    }
    if (n != 1) {
      if (n != 2 && n != 3 && n != 5) {
        return false;
      }
    }
    return true;
  }
};
}  // namespace

TEST(Leetcode, ugly_number) {
  Solution s;
  EXPECT_TRUE(s.isUgly(1));
  EXPECT_TRUE(s.isUgly(6));
  EXPECT_FALSE(s.isUgly(14));
  EXPECT_FALSE(s.isUgly(-1));
}
