#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int singleNumber(const std::vector<int>& nums) {
    int ret = 0;
    for (auto num : nums) {
      ret ^= num;
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, single_number) {
  Solution s;
  EXPECT_EQ(4, s.singleNumber({3, 2, 3, 2, 4}));
  EXPECT_EQ(1, s.singleNumber({2, 1, 2}));
}
