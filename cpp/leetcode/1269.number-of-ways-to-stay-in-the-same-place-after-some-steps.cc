#include <gtest/gtest.h>

#include <vector>

namespace {
constexpr int MOD = 1E9 + 7;
class Solution {
 public:
  int numWays(int arrLen, int steps) {
    // 能够到达的最大下标位置
    int maxcol = std::min(arrLen - 1, steps);
    // dp[i][j] 表示执行i步到达j位置的方案数
    // dp[i][j] = dp[i - 1][j] + dp[i - 1][j + 1] + dp[i - 1][j - 1];
    std::vector<std::vector<int>> dp(steps + 1, std::vector<int>(maxcol + 1));
    dp[0][0] = 1;
    for (int i = 1; i <= steps; ++i) {
      for (int j = 0; j <= maxcol; ++j) {
        dp[i][j] = dp[i - 1][j];
        if (j - 1 >= 0) {
          dp[i][j] = (dp[i][j] + dp[i - 1][j - 1]) % MOD;
        }
        if (j + 1 <= maxcol) {
          dp[i][j] = (dp[i][j] + dp[i - 1][j + 1]) % MOD;
        }
      }
    }
    return dp[steps][0];
  }
};
}  // namespace

TEST(Leetcode, number_of_ways_to_stay_in_the_same_place_after_some_steps) {
  Solution s;
  EXPECT_EQ(4, s.numWays(2, 3));
  EXPECT_EQ(2, s.numWays(4, 2));
}
