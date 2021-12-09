#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int numIslands(std::vector<std::vector<char>>& grid) {
    int nr = grid.size();
    if (nr == 0) return 0;
    int nc = grid[0].size();
    int ret = 0;
    for (int i = 0; i < nr; ++i) {
      for (int j = 0; j < nc; ++j) {
        if (grid[i][j] == '1') {
          ret++;
          dfs(grid, i, j);
        }
      }
    }
    return ret;
  }

 private:
  void dfs(std::vector<std::vector<char>>& grid, int x, int y) {
    int nr = grid.size();
    int nc = grid[0].size();
    grid[x][y] = '0';
    // clang-format off
    if (x - 1 >= 0 && grid[x - 1][y] == '1') dfs(grid, x - 1, y);
    if (x + 1 < nr && grid[x + 1][y] == '1') dfs(grid, x + 1, y);
    if (y - 1 >= 0 && grid[x][y - 1] == '1') dfs(grid, x, y - 1);
    if (y + 1 < nc && grid[x][y + 1] == '1') dfs(grid, x, y + 1);
    // clang-format on
  }
};
}  // namespace

TEST(Leetcode, number_of_islands) {
  Solution s;
  {
    std::vector<std::vector<char>> grid = {
        {'1', '1', '1', '1', '0'},
        {'1', '1', '0', '1', '0'},
        {'1', '1', '0', '0', '0'},
        {'0', '0', '0', '0', '0'},
    };
    EXPECT_EQ(1, s.numIslands(grid));
  }

  {
    std::vector<std::vector<char>> grid = {
        {'1', '1', '0', '0', '0'},
        {'1', '1', '0', '0', '0'},
        {'0', '0', '1', '0', '0'},
        {'0', '0', '0', '1', '1'},
    };
    EXPECT_EQ(3, s.numIslands(grid));
  }
}
