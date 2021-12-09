#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  bool oneEditAway(const std::string& first, const std::string& second) {
    int len1 = first.length();
    int len2 = second.length();
    if (std::abs(len1 - len2) > 1) {
      return false;
    }
    // 如果len1 == len2，只考虑替换
    if (len1 == len2) {
      int diff = 0;
      for (int i = 0; i < len1; ++i) {
        if (first[i] != second[i]) {
          diff++;
        }
        if (diff > 1) {
          return false;
        }
      }
      return true;
    }
    // 如果len1 < len2，只考虑插入
    if (len1 < len2) {
      int skip = 0;
      for (int i = 0, j = 0; i < len1 && j < len2;) {
        if (first[i] != second[j]) {
          skip++;
          j++;
        } else {
          i++;
          j++;
        }
        if (skip > 1) {
          return false;
        }
      }
      return true;
    }

    // 如果len1 > len2，只考虑删除
    // 删除可以认为是second -> first的插入
    if (len1 > len2) {
      return oneEditAway(second, first);
    }
    return true;
  }
};
}  // namespace

TEST(Leetcode, one_way_lcci) {
  Solution s;
  EXPECT_TRUE(s.oneEditAway("pale", "ple"));
  EXPECT_FALSE(s.oneEditAway("pales", "pal"));
}
