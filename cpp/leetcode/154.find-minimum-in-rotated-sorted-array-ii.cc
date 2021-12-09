#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int findMin(const std::vector<int>& nums) {
    int s = 0, e = nums.size() - 1;
    while (s < e) {
      int m = (s + e) / 2;
      if (nums[m] > nums[e]) {
        s = m + 1;
      } else if (nums[m] < nums[e]) {
        e = m;
      } else {
        e--;
      }
    }
    return nums[s];
  }
};
}  // namespace

TEST(Leetcode, find_minimum_in_rotated_sorted_array_ii) {
  Solution s;
  EXPECT_EQ(1, s.findMin({1, 3, 5}));
  EXPECT_EQ(0, s.findMin({2, 2, 2, 0, 1}));
}
