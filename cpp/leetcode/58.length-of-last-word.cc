#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  int lengthOfLastWord(const std::string& s) {
    int i = s.length() - 1;
    int ret = 0;
    // 去末尾空格
    while (i >= 0 && s[i] == ' ') {
      i--;
    }
    while (i >= 0 && s[i] != ' ') {
      ret++;
      i--;
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, length_of_last_word) {
  Solution s;
  EXPECT_EQ(5, s.lengthOfLastWord("  Hello World  "));
  EXPECT_EQ(0, s.lengthOfLastWord(" "));
}
