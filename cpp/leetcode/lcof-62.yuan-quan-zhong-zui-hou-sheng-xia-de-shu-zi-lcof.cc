#include <gtest/gtest.h>

namespace {
class Solution {
 public:
  int lastRemaining(int n, int m) {
    if (n == 1) {
      return 0;
    }
    return (lastRemaining(n - 1, m) + m) % n;
  }
};
}  // namespace

TEST(Leetcode, yuan_quan_zhong_zui_hou_sheng_xia_de_shu_zi_lcof) {
  Solution s;
  EXPECT_EQ(3, s.lastRemaining(5, 3));
  EXPECT_EQ(2, s.lastRemaining(10, 17));
}
