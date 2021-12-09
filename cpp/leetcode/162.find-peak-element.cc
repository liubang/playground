#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int findPeakElement(const std::vector<int>& nums) {
    int s = 0, e = nums.size() - 1;
    while (s < e) {
      int m = (s + e) / 2;
      if (nums[m] > nums[m + 1]) {
        e = m;
      } else {
        s = m + 1;
      }
    }
    return s;
  }
};
}  // namespace

TEST(Leetcode, find_peak_element) {
  Solution s;
  EXPECT_EQ(2, s.findPeakElement({1, 2, 3, 1}));
  EXPECT_EQ(1, s.findPeakElement({1, 2, 1}));
  EXPECT_EQ(0, s.findPeakElement({1}));
  EXPECT_EQ(1, s.findPeakElement({1, 2}));
}
