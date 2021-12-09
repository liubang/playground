#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<std::vector<int>> transpose(
      std::vector<std::vector<int>>& matrix) {
    int m = matrix.size();
    int n = matrix[0].size();
    std::vector<std::vector<int>> ret(n, std::vector<int>(m, 0));
    for (auto i = 0; i < n; ++i) {
      for (auto j = 0; j < m; ++j) {
        ret[i][j] = matrix[j][i];
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, transpose_matrix) {
  Solution s;
  {
    std::vector<std::vector<int>> matrix = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    std::vector<std::vector<int>> exp = {{1, 4, 7}, {2, 5, 8}, {3, 6, 9}};
    auto ret = s.transpose(matrix);
    EXPECT_EQ(exp, ret);
  }

  {
    std::vector<std::vector<int>> matrix = {{1, 2, 3}, {4, 5, 6}};
    std::vector<std::vector<int>> exp = {{1, 4}, {2, 5}, {3, 6}};
    auto ret = s.transpose(matrix);
    EXPECT_EQ(exp, ret);
  }
}
