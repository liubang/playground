#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int minPathSum(const std::vector<std::vector<int>>& grid) {
    int m = grid.size(), n = grid[0].size();
    std::vector<std::vector<int>> dp(m, std::vector<int>(n, 0));
    for (int i = 0; i < m; ++i) {
      for (int j = 0; j < n; ++j) {
        if (i == 0 && j == 0) {
          dp[i][j] = grid[i][j];
        } else if (i == 0) {
          dp[i][j] = dp[i][j - 1] + grid[i][j];
        } else if (j == 0) {
          dp[i][j] = dp[i - 1][j] + grid[i][j];
        } else {
          dp[i][j] = std::min(dp[i][j - 1], dp[i - 1][j]) + grid[i][j];
        }
      }
    }
    return dp[m - 1][n - 1];
  }
};
}  // namespace

TEST(Leetcode, minimum_path_sum) {
  Solution s;
  EXPECT_EQ(7, s.minPathSum({{1, 3, 1}, {1, 5, 1}, {4, 2, 1}}));
  EXPECT_EQ(12, s.minPathSum({{1, 2, 3}, {4, 5, 6}}));
}
