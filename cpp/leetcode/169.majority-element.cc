#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int majorityElement(const std::vector<int>& nums) {
    int ret = nums[0], sum = 0;
    for (int i = 0; i < nums.size(); ++i) {
      if (sum == 0) {
        ret = nums[i];
        sum++;
      } else if (nums[i] == ret) {
        sum++;
      } else {
        sum--;
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, majority_element) {
  Solution s;

  EXPECT_EQ(3, s.majorityElement({3, 2, 3}));
  EXPECT_EQ(2, s.majorityElement({2, 2, 1, 1, 1, 2, 2}));
}
