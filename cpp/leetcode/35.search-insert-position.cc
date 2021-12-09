#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int searchInsert(const std::vector<int>& nums, int target) {
    if (nums.empty() || nums[0] > target) {
      return 0;
    }
    int size = nums.size();
    if (nums[size - 1] < target) {
      return size;
    }
    int s = 0, e = size - 1;
    while (s < e) {
      int m = (s + e) / 2;
      if (nums[m] == target) {
        return m;
      } else if (target > nums[m]) {
        s = m + 1;
      } else {
        e = m;
      }
    }
    return s;
  }
};
}  // namespace

TEST(Leetcode, search_insert_position) {
  Solution s;
  EXPECT_EQ(3, s.searchInsert({1, 3, 5, 8}, 7));
  EXPECT_EQ(0, s.searchInsert({1, 3, 5, 8}, 0));
  EXPECT_EQ(4, s.searchInsert({1, 3, 5, 8}, 10));
}
