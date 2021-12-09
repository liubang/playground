#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int coinChange(const std::vector<int>& coins, int amount) {
    std::vector<int> dp(amount + 1, amount + 1);
    dp[0] = 0;
    for (int i = 1; i <= amount; ++i) {
      for (auto coin : coins) {
        if (coin <= i) {
          dp[i] = std::min(dp[i], dp[i - coin] + 1);
        }
      }
    }
    return dp[amount] > amount ? -1 : dp[amount];
  }
};
}  // namespace

TEST(Leetcode, coin_change) {
  Solution s;
  EXPECT_EQ(3, s.coinChange({1, 2, 5}, 11));
  EXPECT_EQ(-1, s.coinChange({2}, 3));
  EXPECT_EQ(1, s.coinChange({1}, 1));
}
