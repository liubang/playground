#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<std::vector<int>> merge(
      std::vector<std::vector<int>>& intervals) {
    if (intervals.size() <= 1) {
      return intervals;
    }
    std::sort(intervals.begin(), intervals.end(), [](auto& lhs, auto& rhs) {
      return (lhs[0] < rhs[0]) || (lhs[0] == rhs[0] && lhs[1] < rhs[1]);
    });

    std::vector<std::vector<int>> ret;
    int st = intervals[0][0], ed = intervals[0][1];
    for (int i = 1; i < intervals.size(); ++i) {
      if (ed >= intervals[i][0]) {
        ed = std::max(ed, intervals[i][1]);
      } else {
        ret.push_back({st, ed});
        st = intervals[i][0];
        ed = intervals[i][1];
      }
    }
    ret.push_back({st, ed});
    return ret;
  }
};
}  // namespace

TEST(Leetcode, merge_intervals) {
  Solution s;

  {
    std::vector<std::vector<int>> inputs = {{1, 3}, {2, 6}, {8, 10}, {15, 18}};
    std::vector<std::vector<int>> exp = {{1, 6}, {8, 10}, {15, 18}};
    EXPECT_EQ(exp, s.merge(inputs));
  }

  {
    std::vector<std::vector<int>> inputs = {{1, 4}, {4, 5}};
    std::vector<std::vector<int>> exp = {{1, 5}};
    EXPECT_EQ(exp, s.merge(inputs));
  }
}
