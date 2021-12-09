#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  bool canJump(const std::vector<int>& nums) {
    int pre = 0;
    for (int i = 0; i < nums.size(); ++i) {
      if (i > pre) {
        return false;
      }
      pre = std::max(pre, i + nums[i]);
    }
    return true;
  }
};
}  // namespace

TEST(Leetcode, jump_game) {
  Solution s;
  EXPECT_TRUE(s.canJump({1, 2, 3, 4}));
  EXPECT_FALSE(s.canJump({3, 2, 1, 0, 4}));
}
