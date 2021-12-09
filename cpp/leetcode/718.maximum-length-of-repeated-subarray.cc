#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int findLength(const std::vector<int>& A, const std::vector<int>& B) {
    int m = A.size();
    int n = B.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    int ret = 0;
    for (int i = 1; i <= m; ++i) {
      for (int j = 1; j <= n; ++j) {
        if (A[i - 1] == B[j - 1]) {
          dp[i][j] = dp[i - 1][j - 1] + 1;
        }
        if (dp[i][j] > ret) {
          ret = dp[i][j];
        }
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, maximum_length_of_repeated_subarray) {
  Solution s;
  EXPECT_EQ(3, s.findLength({1, 2, 3, 2, 1}, {3, 2, 1, 4, 7}));
}
