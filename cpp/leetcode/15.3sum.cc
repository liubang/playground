#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {
class Solution {
 public:
  std::vector<std::vector<int>> threeSum(std::vector<int>& nums) {
    int size = nums.size();
    if (size < 3) {
      return {};
    }
    std::sort(nums.begin(), nums.end());
    std::vector<std::vector<int>> ret;
    for (int i = 0; i < size; ++i) {
      if (nums[i] > 0) {
        break;
      }
      // 去重
      if (i > 0 && nums[i] == nums[i - 1]) {
        continue;
      }
      int l = i + 1, r = size - 1;
      while (l < r) {
        int sum = nums[i] + nums[l] + nums[r];
        if (sum == 0) {
          // 去重
          while (l < r && nums[l] == nums[l + 1]) {
            l++;
          }
          while (l < r && nums[r] == nums[r - 1]) {
            r--;
          }
          ret.push_back({nums[i], nums[l], nums[r]});
          l++;
          r--;
        } else if (sum > 0) {
          r--;
        } else if (sum < 0) {
          l++;
        }
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, 3sum) {
  Solution s;
  std::vector<std::vector<int>> exp = {
      {-1, -1, 2},
      {-1, 0, 1},
  };
  std::vector<int> input = {-1, 0, 1, 2, -1, -4};
  EXPECT_EQ(exp, s.threeSum(input));
}
