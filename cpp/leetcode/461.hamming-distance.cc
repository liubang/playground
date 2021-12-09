#include <gtest/gtest.h>
namespace {
class Solution {
 public:
  int hammingDistance(int x, int y) {
#if defined(__GNUC__)
    return __builtin_popcount(x ^ y);
#else
    int tmp = x ^ y;
    int ret = 0;
    while (tmp > 0) {
      if ((tmp & 1) == 1) ret++;
      tmp >>= 1;
    }
    return ret;
#endif
  }
};
}  // namespace

TEST(Leetcode, hamming_distance) {
  Solution s;

  EXPECT_EQ(2, s.hammingDistance(1, 4));
  EXPECT_EQ(6, s.hammingDistance(333, 555));
}
