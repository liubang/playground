#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int removeElement(std::vector<int>& nums, int target) {
    for (auto it = nums.begin(); it != nums.end();) {
      if (*it == target) {
        nums.erase(it);
      } else {
        it++;
      }
    }
    return nums.size();
  }
};
}  // namespace

TEST(Leetcode, remove_element) {
  Solution s;

  {
    std::vector<int> inputs = {1, 2, 2, 3, 4};
    EXPECT_EQ(3, s.removeElement(inputs, 2));
    for (auto i : inputs) {
      EXPECT_NE(2, i);
    }
  }
}
