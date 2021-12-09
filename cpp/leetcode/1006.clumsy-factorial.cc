#include <gtest/gtest.h>

namespace {
class Solution {
 public:
  int clumsy(int N) {
    if (N <= 2) {
      return N;
    }
    if (N == 3) {
      return 6;
    }

    int ret = N * (N - 1) / (N - 2) + N - 3;
    N -= 4;

    while (N >= 4) {
      ret += (-N * (N - 1) / (N - 2) + N - 3);
      N -= 4;
    }

    return ret - clumsy(N);
  }
};
}  // namespace

TEST(Leetcode, clumsy_factorial) {
  Solution s;
  EXPECT_EQ(1, s.clumsy(1));
  EXPECT_EQ(2, s.clumsy(2));
  EXPECT_EQ(6, s.clumsy(3));
  EXPECT_EQ(7, s.clumsy(4));
  EXPECT_EQ(8, s.clumsy(6));
  EXPECT_EQ(12, s.clumsy(10));
  EXPECT_EQ(10001, s.clumsy(10000));
}
