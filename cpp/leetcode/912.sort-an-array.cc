#include <vector>
#include <gtest/gtest.h>

namespace {
class Solution {
 public:
  std::vector<int> sortArray(std::vector<int>& nums) {
    quickSort(nums, 0, nums.size() - 1);
    return nums;
  }

 private:
  void quickSort(std::vector<int>& nums, int s, int e) {
    if (s >= e) {
      return;
    }
    int i = s, j = e;
    while (i < j) {
      while (i < j && nums[j] >= nums[i]) {
        --j;
      }
      std::swap(nums[i], nums[j]);
      while (i < j && nums[i] < nums[j]) {
        ++i;
      }
      std::swap(nums[i], nums[j]);
    }
    quickSort(nums, s, i - 1);
    quickSort(nums, i + 1, e);
  }
};
} // namespace

TEST(Leetcode, sort_an_array) {
  Solution s;
  {
    std::vector<int> nums = {5, 4, 3, 2, 1, 1, 2, 3, 6, 7, 8};
    std::vector<int> exps = {1, 1, 2, 2, 3, 3, 4, 5, 6, 7, 8};
    s.sortArray(nums);
    EXPECT_EQ(exps, nums);
  }
  {
    std::vector<int> nums = {1};
    std::vector<int> exps = {1};
    s.sortArray(nums);
    EXPECT_EQ(exps, nums);
  }
}
