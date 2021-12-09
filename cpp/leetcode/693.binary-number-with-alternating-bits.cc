#include <gtest/gtest.h>

namespace {
class Solution {
 public:
  bool hasAlternatingBits(int n) {
    int pre = 1 & n;
    while (n > 0) {
      n = n >> 1;
      if (pre == (1 & n)) {
        return false;
      }
      pre = 1 & n;
    }
    return true;
  }
};
}  // namespace

TEST(Leetcode, binary_number_with_alternating_bits) {
  Solution s;
  EXPECT_TRUE(s.hasAlternatingBits(5));
  EXPECT_FALSE(s.hasAlternatingBits(8));
}
