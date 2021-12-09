#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int trap(const std::vector<int>& height) {
    int size = height.size();
    int r = 0;
    std::vector<int> max(size, 0);
    for (int i = size - 1; i >= 0; --i) {
      max[i] = r;
      r = std::max(r, height[i]);
    }
    int l = 0, ret = 0;
    for (int i = 0; i < size; ++i) {
      int add = std::min(l, max[i]) - height[i];
      ret += add > 0 ? add : 0;
      l = std::max(l, height[i]);
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, volume_of_histogram_lcci) {
  Solution s;
  EXPECT_EQ(6, s.trap({0, 1, 0, 2, 1, 0, 1, 3, 2, 1, 2, 1}));
}
