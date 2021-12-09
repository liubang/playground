#include <gtest/gtest.h>

#include <queue>
#include <vector>

namespace {
class Solution {
 public:
  int findKthLargest(const std::vector<int>& nums, int k) {
    // 小顶堆
    std::priority_queue<int, std::vector<int>, std::greater<int>> queue;
    for (int i = 0; i < nums.size(); ++i) {
      if (i < k) {
        queue.push(nums[i]);
      } else {
        if (nums[i] > queue.top()) {
          queue.pop();
          queue.push(nums[i]);
        }
      }
    }
    return queue.top();
  }
};
}  // namespace

TEST(Leetcode, kth_largest_element_in_an_array) {
  Solution s;
  EXPECT_EQ(5, s.findKthLargest({3, 2, 1, 5, 6, 4}, 2));
  EXPECT_EQ(4, s.findKthLargest({3, 2, 3, 1, 2, 4, 5, 5, 6}, 4));
}
