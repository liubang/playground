#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int findMaxConsecutiveOnes(std::vector<int>& nums) {
    int ret = 0, cur = 0;
    for (auto num : nums) {
      if (num == 0) {
        ret = std::max(ret, cur);
        cur = 0;
      } else {
        cur++;
      }
    }
    return std::max(ret, cur);
  }
};
}  // namespace

TEST(Leetcode, max_consecutive_ones) {
  Solution s;
  {
    std::vector<int> nums = {1, 1, 0, 1, 1, 1};
    auto res = s.findMaxConsecutiveOnes(nums);
    EXPECT_EQ(3, res);
  }
  {
    std::vector<int> nums = {1, 0, 1, 1, 0, 1};
    auto res = s.findMaxConsecutiveOnes(nums);
    EXPECT_EQ(2, res);
  }
}
