#include "common.h"

namespace ds {
class Solution {
 public:
  int lengthOfLIS(std::vector<int>& nums) {
    if (nums.empty()) {
      return 0;
    }
    // dp[i]表示前i个数的最长增长子序列的长度
    std::vector<int> dp(nums.size(), 1);
    for (int i = 1; i < nums.size(); i++) {
      for (int j = 0; j < i; j++) {
        if (nums[j] < nums[i]) {
          // 将前j个最长增长子序列和当前第i个元素构成的子序列长度
          // 和原来的前i个子序列长度取较大的赋值给dp[i]
          dp[i] = std::max(dp[i], dp[j] + 1);
        }
      }
    }
    return *std::max_element(dp.begin(), dp.end());
  }

  int lengthOfLIS2(std::vector<int>& nums) {
    // 还可以直接计算出最长增长子序列，然后返回其长度
    if (nums.size() < 2) {
      return nums.size();
    }
    // dp中保存的是当前子序列位置下的最长增长子序列
    std::vector<int> dp;
    for (auto& num : nums) {
      // 在dp中二分查找当前元素的位置，如果在dp中，则替换idx处的元素为当前元素
      // 如果不在dp中，则将当前元素追加到dp中
      auto idx = std::lower_bound(dp.begin(), dp.end(), num) - dp.begin();
      if (idx >= dp.size()) {
        dp.emplace_back(num);
      } else {
        dp[idx] = num;
      }
    }
    return dp.size();
  }
};
} // namespace ds

TEST(LongestIncreasingSubsequence, lengthOfLIS) {
  ds::Solution s;
  std::vector<int> input = {10, 9, 2, 5, 3, 7, 101, 18};
  EXPECT_EQ(4, s.lengthOfLIS(input));
  std::vector<int> empty_input;
  EXPECT_EQ(0, s.lengthOfLIS(empty_input));
}

TEST(LongestIncreasingSubsequence, lengthOfLIS2) {
  ds::Solution s;
  std::vector<int> input = {10, 9, 2, 5, 3, 7, 101, 18};
  EXPECT_EQ(4, s.lengthOfLIS2(input));
  std::vector<int> empty_input;
  EXPECT_EQ(0, s.lengthOfLIS2(empty_input));
}
