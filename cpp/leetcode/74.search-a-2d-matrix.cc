#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  bool searchMatrix(const std::vector<std::vector<int>>& matrix, int target) {
    int m = matrix.size();
    int n = matrix[0].size();
    for (int i = m - 1, j = 0; (i >= 0) && (j < n);) {
      if (matrix[i][j] == target) {
        return true;
      } else if (matrix[i][j] < target) {
        j++;
      } else {
        i--;
      }
    }
    return false;
  }
};
}  // namespace

TEST(Leetcode, search_a_2d_matrix) {
  Solution s;
  std::vector<std::vector<int>> input = {
      {1, 3, 5, 7}, {10, 11, 16, 20}, {23, 30, 34, 60}};
  for (int i = 0; i < input.size(); ++i) {
    for (int j = 0; j < input[0].size(); ++j) {
      EXPECT_TRUE(s.searchMatrix(input, input[i][j]));
    }
  }

  EXPECT_FALSE(s.searchMatrix(input, 2));
  EXPECT_FALSE(s.searchMatrix(input, 4));
  EXPECT_FALSE(s.searchMatrix(input, 6));
  EXPECT_FALSE(s.searchMatrix(input, 12));
  EXPECT_FALSE(s.searchMatrix(input, 13));
  EXPECT_FALSE(s.searchMatrix(input, 18));
  EXPECT_FALSE(s.searchMatrix(input, 70));
}
