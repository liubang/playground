#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {
class Solution {
 public:
  int minMoves(const std::vector<int>& nums) {
    auto po = std::min_element(nums.begin(), nums.end());
    int ret = 0;
    for (auto num : nums) {
      ret += num - *po;
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, minimum_moves_to_equal_array_elements) {
  Solution s;
  {
    std::vector<int> input = {1, 2, 3};
    auto ret = s.minMoves(input);
    EXPECT_EQ(3, ret);
  }
  {
    std::vector<int> input = {6, 2, 3, 10};
    auto ret = s.minMoves(input);
    EXPECT_EQ(13, ret);
  }
}
