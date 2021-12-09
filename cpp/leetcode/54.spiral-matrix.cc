#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> spiralOrder(const std::vector<std::vector<int>>& matrix) {
    int m = matrix.size();
    int n = matrix[0].size();
    int xmin = 0, xmax = m - 1;
    int ymin = 0, ymax = n - 1;
    int x = 0, y = 0;
    std::vector<int> ret;
    Direction d = Direction::Right;
    for (;;) {
      switch (d) {
        case Direction::Right:
          if (ymin > ymax) return ret;
          for (y = ymin; y <= ymax; ++y) {
            ret.push_back(matrix[xmin][y]);
          }
          xmin++;
          d = Direction::Down;
          break;
        case Direction::Down:
          if (xmin > xmax) return ret;
          for (x = xmin; x <= xmax; ++x) {
            ret.push_back(matrix[x][ymax]);
          }
          ymax--;
          d = Direction::Left;
          break;
        case Direction::Left:
          if (ymin > ymax) return ret;
          for (y = ymax; y >= ymin; --y) {
            ret.push_back(matrix[xmax][y]);
          }
          xmax--;
          d = Direction::Up;
          break;
        case Direction::Up:
          if (xmin > xmax) return ret;
          for (x = xmax; x >= xmin; --x) {
            ret.push_back(matrix[x][ymin]);
          }
          ymin++;
          d = Direction::Right;
          break;
      }
    }
    return ret;
  }

 private:
  enum class Direction {
    Right,
    Down,
    Left,
    Up,
  };
};
}  // namespace

TEST(Leetcode, spiral_matrix) {
  Solution s;
  {
    std::vector<int> exp = {1, 2, 3, 6, 9, 8, 7, 4, 5};
    EXPECT_EQ(exp, s.spiralOrder({{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}));
  }

  {
    std::vector<int> exp = {1, 2, 3, 4, 8, 12, 11, 10, 9, 5, 6, 7};
    EXPECT_EQ(exp,
              s.spiralOrder({{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}}));
  }
}
