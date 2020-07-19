#include "common.h"

namespace ds {
class Solution {
 public:
  int findMinArrowShots(std::vector<std::pair<int, int>>& points) {
    if (points.empty()) {
      return 0;
    }

    std::sort(
        points.begin(),
        points.end(),
        [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
          return a.second < b.second;
        });

    int ret = 1;
    int cur = points.front().second;
    for (int i = 1; i < points.size(); i++) {
      if (points[i].first > cur) {
        cur = points[i].second;
        ret++;
      }
    }
    return ret;
  }
};
} // namespace ds

TEST(MinimumNumberofArrowstoBurstBalloons, findMinArrowShots) {
  ds::Solution s;
  std::vector<std::pair<int, int>> input = {{10,16}, {2, 8}, {1, 6}, {7,12}};
  EXPECT_EQ(2, s.findMinArrowShots(input));
}
