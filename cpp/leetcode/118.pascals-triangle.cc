#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<std::vector<int>> generate(int numRows) {
    std::vector<std::vector<int>> ret;
    for (int i = 0; i < numRows; ++i) {
      std::vector<int> row;
      for (int j = 0; j <= i; ++j) {
        if (i == 0 || j == 0 || j == i) {
          row.push_back(1);
        } else {
          row.push_back(ret[i - 1][j - 1] + ret[i - 1][j]);
        }
      }
      ret.emplace_back(std::move(row));
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, pascals_triangle) {
  Solution s;
  {
    std::vector<std::vector<int>> exps = {
        {1}, {1, 1}, {1, 2, 1}, {1, 3, 3, 1}, {1, 4, 6, 4, 1},
    };
    EXPECT_EQ(exps, s.generate(5));
  }
}
