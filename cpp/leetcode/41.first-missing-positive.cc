#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {
class Solution {
 public:
  int firstMissingPositive(std::vector<int>& nums) {
    std::sort(nums.begin(), nums.end());
    int ret = 1;
    for (auto num : nums) {
      if (num <= 0) {
        continue;
      }
      if (num == ret) {
        ret++;
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, first_missing_positive) {
  Solution s;

  {
    std::vector<int> inputs = {0, 2, 2, 1, 1};
    EXPECT_EQ(3, s.firstMissingPositive(inputs));
  }

  {
    std::vector<int> inputs = {3, 4, -1, 1};
    EXPECT_EQ(2, s.firstMissingPositive(inputs));
  }
}
