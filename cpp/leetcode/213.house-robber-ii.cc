#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int rob(const std::vector<int>& nums) {
    int size = nums.size();
    if (size == 1) {
      return nums[0];
    } else if (size == 2) {
      return std::max(nums[0], nums[1]);
    } else {
      return std::max(robRange(nums, 0, size - 2), robRange(nums, 1, size - 1));
    }
  }

 private:
  int robRange(const std::vector<int>& nums, int start, int end) {
    int pp = nums[start], p = std::max(nums[start], nums[start + 1]);
    for (int i = start + 2; i <= end; ++i) {
      int cur = std::max(p, pp + nums[i]);
      pp = p;
      p = cur;
    }
    return p;
  }
};
}  // namespace

TEST(Leetcode, house_robber_ii) {
  Solution s;
  EXPECT_EQ(103, s.rob({1, 3, 1, 3, 100}));
  EXPECT_EQ(4, s.rob({1, 2, 3, 1}));
}
