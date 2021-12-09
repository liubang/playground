#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int search(const std::vector<int>& nums, int target) {
    int size = nums.size();
    if (size == 0 || target < nums[0] || target > nums[size - 1]) {
      return false;
    }
    int st = 0, ed = size - 1;
    while (st <= ed) {
      int mid = (st + ed) / 2;
      if (nums[mid] == target) {
        return mid;
      } else if (nums[mid] > target) {
        ed = mid - 1;
      } else {
        st = mid + 1;
      }
    }
    return -1;
  }
};
}  // namespace

TEST(Leetcode, binary_search) {
  Solution s;
  EXPECT_EQ(4, s.search({1, 0, 3, 5, 9, 12}, 9));
  EXPECT_EQ(-1, s.search({1, 0, 3, 5, 9, 12}, 2));
}
