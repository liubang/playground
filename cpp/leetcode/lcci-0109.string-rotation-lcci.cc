#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  bool isFlipedString(const std::string& s1, const std::string& s2) {
    if (s1.length() != s2.length()) {
      return false;
    }
    std::string s = s1 + s1;
    return s.find(s2) != std::string::npos;
  }
};
}  // namespace

TEST(Leetcode, string_rotation_lcci) {
  Solution s;
  EXPECT_TRUE(s.isFlipedString("waterbottle", "erbottlewat"));
  EXPECT_FALSE(s.isFlipedString("aa", "abc"));
}
