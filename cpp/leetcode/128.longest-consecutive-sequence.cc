#include <gtest/gtest.h>

#include <unordered_set>
#include <vector>

namespace {
class Solution {
 public:
  int longestConsecutive(const std::vector<int>& nums) {
    std::unordered_set<int> set;
    for (auto num : nums) {
      set.insert(num);
    }
    int ret = 0;
    for (auto it = set.begin(); it != set.end(); ++it) {
      int tmp = *it;
      if (set.count(tmp - 1)) {
        continue;
      }
      int cur = 1;
      while (set.count(tmp + cur)) {
        cur++;
      }
      ret = std::max(ret, cur);
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, longest_consecutive_sequence) {
  Solution s;
  EXPECT_EQ(4, s.longestConsecutive({100, 4, 200, 1, 3, 2}));
  EXPECT_EQ(9, s.longestConsecutive({0, 3, 7, 2, 5, 8, 4, 6, 0, 1}));
  EXPECT_EQ(0, s.longestConsecutive({}));
}
