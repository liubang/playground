#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  int strStr(const std::string& haystack, const std::string& needle) {
    if (needle == "") {
      return -1;
    }
    if (haystack.length() < needle.length()) {
      return 0;
    }
    for (int i = 0; i < haystack.length() - needle.length() + 1; ++i) {
      if (haystack[i] == needle[0]) {
        int j = 1;
        while (j < needle.length() && haystack[i + j] == needle[j]) {
          j++;
        }
        if (j == needle.length()) {
          return i;
        }
      }
    }
    return -1;
  }
};
}  // namespace

TEST(Leetcode, implement_strstr) {
  Solution s;
  EXPECT_EQ(2, s.strStr("cccaaaaaaa", "ca"));
  EXPECT_EQ(3, s.strStr("hello", "lo"));
}
