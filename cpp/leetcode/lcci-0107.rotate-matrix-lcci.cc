#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  // m[i][j] -> m[j][n - 1 - i]
  void rotate(std::vector<std::vector<int>>& matrix) {
    int level = matrix.size();
    if (level == 0) {
      return;
    }
    int r = (level >> 1) - 1;
    int c = (level - 1) >> 1;
    for (int i = r; i >= 0; --i) {
      for (int j = c; j >= 0; --j) {
        std::swap(matrix[i][j], matrix[j][level - 1 - i]);
        std::swap(matrix[i][j], matrix[level - 1 - i][level - 1 - j]);
        std::swap(matrix[i][j], matrix[level - 1 - j][i]);
      }
    }
  }
};
}  // namespace

TEST(Leetcode, rotate_matrix_lcci) {
  Solution s;
  {
    std::vector<std::vector<int>> input = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    std::vector<std::vector<int>> output = {{7, 4, 1}, {8, 5, 2}, {9, 6, 3}};
    s.rotate(input);
    EXPECT_EQ(output, input);
  }

  {
    std::vector<std::vector<int>> input = {
        {5, 1, 9, 11}, {2, 4, 8, 10}, {13, 3, 6, 7}, {15, 14, 12, 16}};
    std::vector<std::vector<int>> output = {
        {15, 13, 2, 5}, {14, 3, 4, 1}, {12, 6, 8, 9}, {16, 7, 10, 11}};
    s.rotate(input);
    EXPECT_EQ(output, input);
  }
}
