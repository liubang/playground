#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int findTargetSumWays(const std::vector<int>& nums, int target) {
    int sum = 0;
    std::for_each(nums.begin(), nums.end(), [&sum](int num) { sum += num; });
    if (sum < target || ((sum + target) & 1) == 1) {
      return 0;
    }
    int w = (sum + target) / 2;
    std::vector<int> dp(w + 1);
    dp[0] = 1;
    for (int num : nums) {
      for (int j = w; j >= num; --j) {
        dp[j] += dp[j - num];
      }
    }
    return dp[w];
  }
};
}  // namespace

TEST(Leetcode, target_sum) {
  Solution s;
  EXPECT_EQ(5, s.findTargetSumWays({1, 1, 1, 1, 1}, 3));
  EXPECT_EQ(1, s.findTargetSumWays({1}, 1));
}
