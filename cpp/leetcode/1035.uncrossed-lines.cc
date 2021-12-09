#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int maxUncrossedLines(const std::vector<int>& nums1,
                        const std::vector<int>& nums2) {
    int len1 = nums1.size(), len2 = nums2.size();
    std::vector<std::vector<int>> dp(len1 + 1, std::vector<int>(len2 + 1));
    for (int i = 1; i <= len1; ++i) {
      for (int j = 1; j <= len2; ++j) {
        if (nums1[i - 1] == nums2[j - 1]) {
          dp[i][j] = dp[i - 1][j - 1] + 1;
        } else {
          dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
        }
      }
    }
    return dp[len1][len2];
  }
};
}  // namespace

TEST(Leetcode, uncrossed_lines) {
  Solution s;

  EXPECT_EQ(2, s.maxUncrossedLines({1, 4, 2}, {1, 2, 4}));
  EXPECT_EQ(3, s.maxUncrossedLines({2, 5, 1, 2, 5}, {10, 5, 2, 1, 5, 2}));
  EXPECT_EQ(2, s.maxUncrossedLines({1, 3, 7, 1, 7, 5}, {1, 9, 2, 5, 1}));
}
