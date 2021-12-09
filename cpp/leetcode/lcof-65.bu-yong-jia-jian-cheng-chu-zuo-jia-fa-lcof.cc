#include <gtest/gtest.h>

namespace {
class Solution {
 public:
  int add(int a, int b) {
    return b == 0 ? a : add(a ^ b, (unsigned int)(a & b) << 1);
  }
};
}  // namespace

TEST(Leetcode, bu_yong_jia_jian_cheng_chu_zuo_jia_fa_lcof) {
  Solution s;
  EXPECT_EQ(3, s.add(1, 2));
  EXPECT_EQ(10, s.add(5, 5));
}
