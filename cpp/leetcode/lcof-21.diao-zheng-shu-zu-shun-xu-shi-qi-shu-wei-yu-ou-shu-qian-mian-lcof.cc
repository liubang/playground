#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> exchange(std::vector<int>& nums) {
    if (nums.empty()) {
      return nums;
    }
    int i = 0, j = nums.size() - 1;
    while (i < j) {
      if (i < j && (nums[i] & 1) == 1) {
        i++;
      }
      if (i < j && (nums[j] & 1) == 0) {
        j--;
      }
      if (i < j) {
        std::swap(nums[i], nums[j]);
      }
    }
    return nums;
  }
};
}  // namespace

TEST(Leetcode,
     diao_zheng_shu_zu_shun_xu_shi_qi_shu_wei_yu_ou_shu_qian_mian_lcof) {
  Solution s;

  {
    std::vector<int> inputs = {1, 2, 3, 4, 5};
    auto ret = s.exchange(inputs);
    for (int i = 0; i < ret.size(); ++i) {
      if (i < 3) {
        EXPECT_TRUE((ret[i] & 1) == 1);
      } else {
        EXPECT_TRUE((ret[i] & 1) == 0);
      }
    }
  }
}
