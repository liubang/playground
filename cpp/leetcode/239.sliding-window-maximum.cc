#include <gtest/gtest.h>

#include <queue>
#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> maxSlidingWindow(const std::vector<int>& nums, int k) {
    if (nums.empty()) {
      return {};
    }
    std::priority_queue<std::pair<int, int>> queue;
    std::vector<int> ret;
    int i = 0;
    for (; i < k; ++i) {
      queue.emplace(nums[i], i);
    }
    ret.push_back(queue.top().first);
    for (; i < nums.size(); ++i) {
      queue.emplace(nums[i], i);
      while (queue.top().second <= i - k) {
        queue.pop();
      }
      ret.push_back(queue.top().first);
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, slidding_window_maximum) {
  Solution s;
  {
    std::vector<int> exp = {3, 3, 5, 5, 6, 7};
    EXPECT_EQ(exp, s.maxSlidingWindow({1, 3, -1, -3, 5, 3, 6, 7}, 3));
  }
  {
    std::vector<int> exp = {11};
    EXPECT_EQ(exp, s.maxSlidingWindow({9, 11}, 2));
  }
}
