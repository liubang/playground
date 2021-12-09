#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> constructArr(const std::vector<int>& a) {
    int len = a.size();
    if (len == 0) {
      return {};
    }
    std::vector<int> ret(len, 1);
    for (int i = 1; i < len; ++i) {
      ret[i] = ret[i - 1] * a[i - 1];
    }
    int tmp = 1;
    for (int i = len - 2; i >= 0; --i) {
      tmp *= a[i + 1];
      ret[i] *= tmp;
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, gou_jian_cheng_ji_shu_zu_ocof) {
  Solution s;
  {
    std::vector<int> exp = {120, 60, 40, 30, 24};
    EXPECT_EQ(exp, s.constructArr({1, 2, 3, 4, 5}));
  }
}
