#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> singleNumbers(const std::vector<int>& nums) {
    int ret = 0;
    for (auto num : nums) {
      ret ^= num;
    }
    int a = 0, b = 0;
    int mask = ret & (-ret);
    for (auto num : nums) {
      if (num & mask) {
        a ^= num;
      } else {
        b ^= num;
      }
    }
    return {a, b};
  }
};
}  // namespace

TEST(Leetcode, shu_zu_zhong_shu_zi_chu_xian_de_ci_shu_lcof) {
  Solution s;
  {
    auto ret = s.singleNumbers({1, 2, 10, 4, 1, 4, 3, 3});
    EXPECT_TRUE(ret[0] == 10 || ret[0] == 2);
    EXPECT_TRUE(ret[1] == 10 || ret[1] == 2);
  }
  {
    auto ret = s.singleNumbers({4, 1, 4, 6});
    EXPECT_TRUE(ret[0] == 1 || ret[0] == 6);
    EXPECT_TRUE(ret[1] == 1 || ret[1] == 6);
  }
}
