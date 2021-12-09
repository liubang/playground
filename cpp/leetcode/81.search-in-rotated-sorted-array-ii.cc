#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  bool search(const std::vector<int>& nums, int target) {
    int size = nums.size();
    if (size == 1) {
      return nums[0] == target;
    }
    int i = 0;
    for (; i < size; ++i) {
      if (nums[i] < nums[i - 1]) {
        break;
      }
    }
    // [0, i - 1], [i, size - 1]
    // 现在第一段中查找
    if (target >= nums[0] && target <= nums[i - 1]) {
      int st = 0, ed = i - 1;
      while (st <= ed) {
        int mid = (st + ed) / 2;
        if (target == nums[mid]) {
          return true;
        } else if (target > nums[mid]) {
          st = mid + 1;
        } else {
          ed = mid - 1;
        }
      }
    }

    if (i < size && target >= nums[i] && target <= nums[size - 1]) {
      int st = i, ed = size - 1;
      while (st <= ed) {
        int mid = (st + ed) / 2;
        if (target == nums[mid]) {
          return true;
        } else if (target > nums[mid]) {
          st = mid + 1;
        } else {
          ed = mid - 1;
        }
      }
    }
    return false;
  }
};
}  // namespace

TEST(Leetcode, search_in_rotated_sorted_array_ii) {
  Solution s;
  EXPECT_TRUE(s.search({2, 5, 6, 0, 0, 1, 2}, 0));
  EXPECT_FALSE(s.search({2, 5, 6, 0, 0, 1, 2}, 3));
}
