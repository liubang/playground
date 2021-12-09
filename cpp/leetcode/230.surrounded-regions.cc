#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  // 先将与边界相连的'O'设置为'#'，然后将所有不为'#'的设置为'X'
  // 最后将'#'还原为'O'
  void solve(std::vector<std::vector<char>>& board) {
    // clang-format off
    int nr = board.size();
    if (nr == 0) return;
    int nc = board[0].size();
    for (int i = 0; i < nr; ++i) {
      if (board[i][0] == 'O') dfs(board, i, 0);
      if (board[i][nc - 1] == 'O') dfs(board, i, nc - 1);
    }
    for (int j = 0; j < nc; ++j) {
      if (board[0][j] == 'O') dfs(board, 0, j);
      if (board[nr - 1][j] == 'O') dfs(board, nr - 1, j);
    }
    for (int i = 0; i < nr; ++i) {
      for (int j = 0; j < nc; ++j) {
        if (board[i][j] == '#') board[i][j] = 'O';
        else board[i][j] = 'X';
      }
    }
    // clang-format on
  }

 private:
  void dfs(std::vector<std::vector<char>>& board, int x, int y) {
    int nr = board.size();
    int nc = board[0].size();
    board[x][y] = '#';
    // clang-format off
    if (x - 1 >= 0 && board[x - 1][y] == 'O') dfs(board, x - 1, y);
    if (x + 1 < nr && board[x + 1][y] == 'O') dfs(board, x + 1, y);
    if (y - 1 >= 0 && board[x][y - 1] == 'O') dfs(board, x, y - 1);
    if (y + 1 < nc && board[x][y + 1] == 'O') dfs(board, x, y + 1);
    // clang-format on
  }
};
}  // namespace

TEST(Leetcode, surrounded_regions) {
  Solution s;
  {
    std::vector<std::vector<char>> board = {
        {'X', 'X', 'X', 'X'},
        {'X', 'O', 'O', 'X'},
        {'X', 'X', 'O', 'X'},
        {'X', 'O', 'X', 'X'},
    };
    std::vector<std::vector<char>> exp = {
        {'X', 'X', 'X', 'X'},
        {'X', 'X', 'X', 'X'},
        {'X', 'X', 'X', 'X'},
        {'X', 'O', 'X', 'X'},
    };

    s.solve(board);
    EXPECT_EQ(exp, board);
  }
}
