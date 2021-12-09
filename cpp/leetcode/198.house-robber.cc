#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int rob(const std::vector<int>& nums) {
    int size = nums.size();
    if (size == 0) {
      return 0;
    } else if (size == 1) {
      return nums[0];
    } else if (size == 2) {
      return std::max(nums[0], nums[1]);
    } else {
      int pp = nums[0], p = std::max(nums[0], nums[1]);
      for (int i = 2; i < size; ++i) {
        int cur = std::max(pp + nums[i], p);
        pp = p;
        p = cur;
      }
      return p;
    }
  }
};
}  // namespace

TEST(Leetcode, house_robber) {
  Solution s;
  EXPECT_EQ(4, s.rob({1, 2, 3, 1}));
  EXPECT_EQ(12, s.rob({2, 7, 9, 3, 1}));
}
