#include <gtest/gtest.h>

#include <climits>
#include <vector>

namespace {
class Solution {
 public:
  int maxProfit(const std::vector<int>& prices) {
    if (prices.empty()) {
      return 0;
    }
    int cost = INT_MAX, profit = 0;
    for (int i : prices) {
      cost = std::min(cost, i);
      profit = std::max(profit, i - cost);
    }
    return profit;
  }
};
}  // namespace

TEST(Leetcode, best_time_to_by_and_sell_stock) {
  Solution s;
  EXPECT_EQ(5, s.maxProfit({7, 1, 5, 3, 6, 4}));
  EXPECT_EQ(0, s.maxProfit({7, 6, 4, 3, 1}));
}
