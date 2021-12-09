#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int uniquePaths(int m, int n) {
    std::vector<std::vector<int>> dp(m, std::vector<int>(n, 0));
    for (int i = 0; i < m; ++i) {
      for (int j = 0; j < n; ++j) {
        dp[i][j] = (i == 0 || j == 0) ? 1 : dp[i - 1][j] + dp[i][j - 1];
      }
    }
    return dp[m - 1][n - 1];
  }
};
}  // namespace

TEST(Leetcode, unique_paths) {
  Solution s;
  EXPECT_EQ(28, s.uniquePaths(3, 7));
  EXPECT_EQ(3, s.uniquePaths(3, 2));
  EXPECT_EQ(5005, s.uniquePaths(10, 7));
}
