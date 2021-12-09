#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int maxProfit(const std::vector<int>& prices) {
    int size = prices.size();
    int ret = 0;
    for (int i = 1; i < size; ++i) {
      if (prices[i] > prices[i - 1]) {
        ret += prices[i] - prices[i - 1];
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, best_time_to_buy_and_sell_stock_ii) {
  Solution s;
  EXPECT_EQ(7, s.maxProfit({7, 1, 5, 3, 6, 4}));
  EXPECT_EQ(4, s.maxProfit({1, 2, 3, 4, 5}));
  EXPECT_EQ(0, s.maxProfit({7, 6, 4, 3, 1}));
  EXPECT_EQ(0, s.maxProfit({3, 3}));
}
