#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> decode(const std::vector<int>& encoded, int first) {
    std::vector<int> ret;
    ret.push_back(first);
    for (auto num : encoded) {
      first = first ^ num;
      ret.push_back(first);
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, decode_xored_array) {
  Solution s;
  {
    std::vector<int> exp = {4, 2, 0, 7, 4};
    EXPECT_EQ(exp, s.decode({6, 2, 7, 3}, 4));
  }
  {
    std::vector<int> exp = {1, 0, 2, 1};
    EXPECT_EQ(exp, s.decode({1, 2, 3}, 1));
  }
}
