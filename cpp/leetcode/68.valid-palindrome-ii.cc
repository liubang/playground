#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  bool validPalindrome(const std::string& str) {
    return valid(str, 0, str.length() - 1, true);
  }

 private:
  bool valid(const std::string& str, int s, int e, bool canDelete) {
    while (s < e) {
      if (str[s] != str[e]) {
        if (!canDelete) {
          return false;
        }
        return valid(str, s + 1, e, false) || valid(str, s, e - 1, false);
      }
      s++;
      e--;
    }
    return true;
  }
};
}  // namespace

TEST(Leetcode, valid_palindrome_ii) {
  Solution s;
  EXPECT_TRUE(s.validPalindrome(
      "aguokepatgbnvfqmgmlcupuufxoohdfpgjdmysgvhmvffcnqxjjxqncffvmhvgsymd"
      "jgpfdhooxfuupuculmgmqfvnbgtapekouga"));
  EXPECT_FALSE(s.validPalindrome("abcda"));
}
