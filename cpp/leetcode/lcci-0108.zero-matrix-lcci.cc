#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  void setZeroes(std::vector<std::vector<int>>& matrix) {
    if (matrix.empty()) {
      return;
    }
    int m = matrix.size();
    int n = matrix[0].size();
    std::vector<std::vector<int>> copy = matrix;
    for (int i = 0; i < m; ++i) {
      for (int j = 0; j < n; ++j) {
        if (copy[i][j] == 0) {
          for (int x = 0; x < m; ++x) {
            matrix[x][j] = 0;
          }
          for (int y = 0; y < n; ++y) {
            matrix[i][y] = 0;
          }
        }
      }
    }
  }
};
}  // namespace

TEST(Leetcode, zero_matrix_lcci) {
  Solution s;
  {
    std::vector<std::vector<int>> input = {{1, 1, 1}, {1, 0, 1}, {1, 1, 1}};
    std::vector<std::vector<int>> exp = {{1, 0, 1}, {0, 0, 0}, {1, 0, 1}};
    s.setZeroes(input);
    EXPECT_EQ(exp, input);
  }

  {
    std::vector<std::vector<int>> input = {
        {0, 1, 2, 0}, {3, 4, 5, 2}, {1, 3, 1, 5}};
    std::vector<std::vector<int>> exp = {
        {0, 0, 0, 0}, {0, 4, 5, 0}, {0, 3, 1, 0}};
    s.setZeroes(input);
    EXPECT_EQ(exp, input);
  }
}
