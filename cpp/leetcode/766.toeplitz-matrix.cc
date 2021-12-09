#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  bool isToeplitzMatrix(std::vector<std::vector<int>>& matrix) {
    auto m = matrix.size();
    auto n = matrix[0].size();
    for (auto i = 1; i < m; ++i) {
      for (auto j = 1; j < n; ++j) {
        if (matrix[i][j] != matrix[i - 1][j - 1]) return false;
      }
    }
    return true;
  }
};
}  // namespace

TEST(Leetcode, toeplitz_matrix) {
  Solution s;
  {
    std::vector<std::vector<int>> matrix = {
        {1, 2, 3, 4},
        {5, 1, 2, 3},
        {9, 5, 1, 2},
    };
    auto res = s.isToeplitzMatrix(matrix);
    EXPECT_TRUE(res);
  }

  {
    std::vector<std::vector<int>> matrix = {
        {1, 2},
        {2, 2},
    };
    auto res = s.isToeplitzMatrix(matrix);
    EXPECT_FALSE(res);
  }
}
