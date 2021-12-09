#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  std::string compressString(const std::string& S) {
    int len = S.length();
    if (len <= 1) {
      return S;
    }
    std::string ret;
    char pre = S[0];
    int count = 1;
    for (int i = 1; i < len; ++i) {
      if (S[i] != pre) {
        ret.push_back(pre);
        ret.append(std::to_string(count));
        pre = S[i];
        count = 1;
      } else {
        count++;
      }
    }
    ret.push_back(pre);
    ret.append(std::to_string(count));
    return ret.length() < len ? ret : S;
  }
};
}  // namespace

TEST(Leetcode, compress_string_lcci) {
  Solution s;
  EXPECT_EQ("a2b1c5a3", s.compressString("aabcccccaaa"));
  EXPECT_EQ("abbccd", s.compressString("abbccd"));
}
