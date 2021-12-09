#include <gtest/gtest.h>

namespace {
class Solution {
 public:
  int xorOperation(int n, int start) {
    int ret = start;
    for (int i = 1; i < n; ++i) {
      ret ^= (start + i * 2);
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, xor_operation_in_an_array) {
  Solution s;
  EXPECT_EQ(8, s.xorOperation(5, 0));
  EXPECT_EQ(8, s.xorOperation(4, 3));
  EXPECT_EQ(7, s.xorOperation(1, 7));
  EXPECT_EQ(2, s.xorOperation(10, 5));
}
