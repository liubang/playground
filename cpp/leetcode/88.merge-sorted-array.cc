#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {
class Solution {
 public:
  void merge(std::vector<int>& nums1, int m, const std::vector<int>& nums2,
             int n) {
    for (int i = 0; i < n; ++i) {
      nums1[m + i] = nums2[i];
    }
    std::sort(nums1.begin(), nums1.end());
  }
};
}  // namespace

TEST(Leetcode, merge_sorted_array) {
  Solution s;
  std::vector<int> n1 = {1, 2, 3, 0, 0, 0};
  std::vector<int> n2 = {2, 5, 6};
  std::vector<int> exp = {1, 2, 2, 3, 5, 6};
  s.merge(n1, 3, n2, 3);
  EXPECT_EQ(exp, n1);
}
