#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> getRow(int rowIndex) {
    std::vector<int> cur;
    for (int i = 0; i <= rowIndex; ++i) {
      std::vector<int> tmp;
      for (int j = 0; j <= i; ++j) {
        if (i == 0 || j == 0 || i == j) {
          tmp.push_back(1);
        } else {
          tmp.push_back(cur[j] + cur[j - 1]);
        }
      }
      cur = tmp;
    }
    return cur;
  }
};
}  // namespace

TEST(Leetcode, pascals_triangle_ii) {
  Solution s;
  EXPECT_EQ(std::vector<int>({1}), s.getRow(0));
  EXPECT_EQ(std::vector<int>({1, 1}), s.getRow(1));
  EXPECT_EQ(std::vector<int>({1, 2, 1}), s.getRow(2));
  EXPECT_EQ(std::vector<int>({1, 3, 3, 1}), s.getRow(3));
}
