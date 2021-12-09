#include <gtest/gtest.h>

#include <climits>
#include <stack>
#include <vector>

namespace {
class Solution {
 public:
  bool find132pattern(const std::vector<int>& nums) {
    if (nums.size() < 3) {
      return false;
    }
    int two = INT_MIN;
    std::stack<int> st;
    for (int i = nums.size() - 1; i >= 0; --i) {
      if (nums[i] >= two) {
        while (!st.empty() && nums[st.top()] < nums[i]) {
          two = nums[st.top()];
          st.pop();
        }
        st.push(i);
      } else {
        return true;
      }
    }
    return false;
  }
};
}  // namespace

TEST(Leetcode, 132_pattern) {
  Solution s;
  EXPECT_TRUE(s.find132pattern({3, 1, 4, 2}));
  EXPECT_FALSE(s.find132pattern({1, 2, 3, 4}));
  EXPECT_FALSE(s.find132pattern({3, 1}));
}
