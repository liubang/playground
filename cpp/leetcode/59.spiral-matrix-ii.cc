#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<std::vector<int>> generateMatrix(int n) {
    std::vector<std::vector<int>> ret(n, std::vector<int>(n, 0));
    int c = 1, m = 0;
    while (c <= n * n) {
      // 右
      for (int i = m; i < n - m; ++i) {
        ret[m][i] = c++;
      }
      // 下
      for (int i = m + 1; i < n - m; ++i) {
        ret[i][n - m - 1] = c++;
      }
      // 左
      for (int i = n - m - 2; i >= m; --i) {
        ret[n - m - 1][i] = c++;
      }
      // 上
      for (int i = n - m - 2; i > m; --i) {
        ret[i][m] = c++;
      }
      m++;
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, spiral_matrix_ii) {
  Solution s;
  {
    std::vector<std::vector<int>> exps = {{1, 2, 3}, {8, 9, 4}, {7, 6, 5}};
    EXPECT_EQ(exps, s.generateMatrix(3));
  }

  {
    std::vector<std::vector<int>> exps = {{1}};
    EXPECT_EQ(exps, s.generateMatrix(1));
  }
}
