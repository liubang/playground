#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int removeDuplicates(std::vector<int>& nums) {
    int len = 0;
    for (int i = 0; i < nums.size(); ++i) {
      if (len < 2 || nums[i] != nums[len - 2]) {
        nums[len++] = nums[i];
      }
    }
    return len;
  }
};
}  // namespace

TEST(Leetcode, remove_duplicates_from_sorted_array_ii) {
  Solution s;
  {
    std::vector<int> inputs = {1, 1, 1, 2, 2, 3};
    std::vector<int> exp = {1, 1, 2, 2, 3};
    int ret = s.removeDuplicates(inputs);
    EXPECT_EQ(5, ret);
    for (int i = 0; i < ret; ++i) {
      EXPECT_EQ(exp[i], inputs[i]);
    }
  }

  {
    std::vector<int> inputs = {0, 0, 1, 1, 1, 1, 2, 3, 3};
    std::vector<int> exp = {0, 0, 1, 1, 2, 3, 3};
    int ret = s.removeDuplicates(inputs);
    EXPECT_EQ(7, ret);
    for (int i = 0; i < ret; ++i) {
      EXPECT_EQ(exp[i], inputs[i]);
    }
  }
}
