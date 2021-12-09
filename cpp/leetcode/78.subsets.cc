#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<std::vector<int>> subsets(const std::vector<int>& nums) {
    std::vector<std::vector<int>> ret;
    ret.push_back({});
    for (int i = 0; i < nums.size(); ++i) {
      int all = ret.size();
      for (int j = 0; j < all; ++j) {
        std::vector<int> retj = ret[j];
        retj.push_back(nums[i]);
        ret.emplace_back(std::move(retj));
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, subsets) {
  Solution s;
  std::vector<std::vector<int>> exp = {{},  {1},    {2},    {1, 2},
                                       {3}, {1, 3}, {2, 3}, {1, 2, 3}};
  auto ret = s.subsets({1, 2, 3});
  EXPECT_EQ(exp, ret);
}
