#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> sortArray(std::vector<int>& nums) {
    // quickSort(nums, 0, nums.size() - 1);
    mergeSort(nums, 0, nums.size() - 1);
    return nums;
  }

 private:
  void merge(std::vector<int>& nums, int s, int m, int e) {
    // 将数组nums的[s, m]和[m + 1, e]区间的数据合并
    // 到tmp，并且保证tmp有序，然后再拷贝回nums
    int pb = 0;
    int p1 = s, p2 = m + 1;
    std::vector<int> tmp(e - s + 1);
    while (p1 <= m && p2 <= e) {
      if (nums[p1] < nums[p2]) {
        tmp[pb++] = nums[p1++];
      } else {
        tmp[pb++] = nums[p2++];
      }
    }
    while (p1 <= m) {
      tmp[pb++] = nums[p1++];
    }
    while (p2 <= e) {
      tmp[pb++] = nums[p2++];
    }
    for (int i = 0; i < e - s + 1; ++i) {
      nums[s + i] = tmp[i];
    }
  }

  void mergeSort(std::vector<int>& nums, int s, int e) {
    if (s >= e) {
      return;
    }
    // 找到中间位置
    int m = s + (e - s) / 2;
    mergeSort(nums, s, m);
    mergeSort(nums, m + 1, e);
    merge(nums, s, m, e);
  }

  void quickSort(std::vector<int>& nums, int s, int e) {
    if (s >= e) return;
    int i = s, j = e;
    while (i < j) {
      while (i < j && nums[j] >= nums[i]) --j;
      std::swap(nums[i], nums[j]);
      while (i < j && nums[i] < nums[j]) ++i;
      std::swap(nums[i], nums[j]);
    }
    quickSort(nums, s, i - 1);
    quickSort(nums, i + 1, e);
  }
};
}  // namespace

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
