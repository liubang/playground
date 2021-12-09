#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  std::string reverseWords(const std::string& s) {
    int len = s.length();
    if (len == 0 || len == 1) {
      return s;
    }
    std::string ret;
    int i = 0;
    while (s[i] == ' ' && i < len) {
      i++;
    }
    for (; i < len; ++i) {
      std::string tmp;
      while (s[i] != ' ' && i < len) {
        tmp.push_back(s[i++]);
      }
      if (!tmp.empty()) {
        tmp.push_back(' ');
        ret.insert(0, tmp);
      }
    }
    return ret.erase(ret.find_last_not_of(' ') + 1);
  }
};
}  // namespace

TEST(Leetcode, reverse_words_in_a_string) {
  Solution s;
  {
    std::string input = "  hello world!  ";
    std::string exp = "world! hello";
    EXPECT_EQ(exp, s.reverseWords(input));
  }

  {
    std::string input = "a good   example";
    std::string exp = "example good a";
    EXPECT_EQ(exp, s.reverseWords(input));
  }
}
