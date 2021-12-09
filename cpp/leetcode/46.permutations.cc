#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<std::vector<int>> permute(std::vector<int>& nums) {
    std::vector<std::vector<int>> ret;
    dfs(&ret, nums, 0, nums.size() - 1);
    return ret;
  }

 private:
  void dfs(std::vector<std::vector<int>>* ret, std::vector<int>& nums,
           int start, int end) {
    if (start == end) {
      ret->emplace_back(nums.begin(), nums.end());
    } else {
      for (int i = start; i <= end; ++i) {
        std::swap(nums[start], nums[i]);
        dfs(ret, nums, start + 1, end);
        std::swap(nums[start], nums[i]);
      }
    }
  }
};
}  // namespace

TEST(Leetcode, permutations) {
  Solution s;
  std::vector<int> input = {1, 2, 3};
  std::vector<std::vector<int>> exp = {
      {1, 2, 3}, {1, 3, 2}, {2, 1, 3}, {2, 3, 1}, {3, 2, 1}, {3, 1, 2},
  };
  EXPECT_EQ(exp, s.permute(input));
}
