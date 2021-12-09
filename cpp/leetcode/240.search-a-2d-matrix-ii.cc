#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  bool searchMatrix(const std::vector<std::vector<int>>& matrix, int target) {
    if (matrix.empty() || matrix[0].empty() || target < matrix[0][0] ||
        target > matrix[matrix.size() - 1][matrix[0].size() - 1]) {
      return false;
    }
    int m = matrix.size();
    int n = matrix[0].size();
    int i = m - 1, j = 0;
    while (i >= 0 && j < n) {
      if (matrix[i][j] == target) {
        return true;
      } else if (matrix[i][j] > target) {
        i--;
      } else {
        j++;
      }
    }
    return false;
  }
};
}  // namespace

TEST(Leetcode, search_a_2d_matrix_ii) {
  Solution s;
  {
    std::vector<std::vector<int>> input = {{1, 4, 7, 11, 15},
                                           {2, 5, 8, 12, 19},
                                           {3, 6, 9, 16, 22},
                                           {10, 13, 14, 17, 24},
                                           {18, 21, 23, 26, 30}};
    EXPECT_TRUE(s.searchMatrix(input, 5));
    EXPECT_FALSE(s.searchMatrix(input, 33));
  }

  {
    std::vector<std::vector<int>> input = {{1, 4, 7, 11, 15},
                                           {2, 5, 8, 12, 19},
                                           {3, 6, 9, 16, 22},
                                           {10, 13, 14, 17, 24},
                                           {18, 21, 23, 26, 30}};
    EXPECT_FALSE(s.searchMatrix(input, 20));
  }
}
