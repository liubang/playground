#include "common.h"

namespace ds {
class Solution {
 private:
  const std::vector<std::pair<int, int>> points = {{-1, -1},
                                                   {-1, 0},
                                                   {-1, 1},
                                                   {0, -1},
                                                   {0, 1},
                                                   {1, -1},
                                                   {1, 0},
                                                   {1, 1},
                                                   {0, 0}};

 public:
  // 如果活细胞周围八个位置的活细胞数少于两个，则该位置活细胞死亡；
  // 如果活细胞周围八个位置有两个或三个活细胞，则该位置活细胞仍然存活；
  // 如果活细胞周围八个位置有超过三个活细胞，则该位置活细胞死亡；
  // 如果死细胞周围正好有三个活细胞，则该位置死细胞复活；
  void gameOfLife(std::vector<std::vector<int>>& board) {
    int m = board.size();
    int n = m ? board[0].size() : 0;
    for (int i = 0; i < m; i++) {
      for (int j = 0; j < n; j++) {
        // scan 3 * 3 region and compute total livies.
        int livies = 0;
        for (auto& point : points) {
          if (i + point.first < 0 || i + point.first >= m ||
              j + point.second < 0 || j + point.second >= n) {
            continue;
          }
          livies += (board[i + point.first][j + point.second] & 1);
        }
        if (livies == 3 || livies - board[i][j] == 3) {
          board[i][j] |= 0b10;
        }
      }
    }

    for (int i = 0; i < m; i++) {
      for (int j = 0; j < n; j++) {
        board[i][j] = board[i][j] >> 1;
      }
    }
  }
};
} // namespace ds

TEST(GameOfLife, gameOfLife) {
  ds::Solution s;
  std::vector<std::vector<int>> input = {
      {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {0, 0, 0}};
  s.gameOfLife(input);

  std::vector<std::vector<int>> exp = {
      {0, 0, 0}, {1, 0, 1}, {0, 1, 1}, {0, 1, 0}};

  EXPECT_EQ(exp, input);
}
