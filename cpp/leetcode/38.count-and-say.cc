#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  std::string countAndSay(int n) {
    std::string cur = "1";
    if (n == 1) {
      return cur;
    }
    for (int i = 2; i <= n; ++i) {
      std::string tmp;
      for (int j = 0; j < cur.length();) {
        int m = j;
        while (m < cur.length() && cur[m] == cur[j]) {
          m++;
        }
        tmp.push_back((m - j) + '0');
        tmp.push_back(cur[m - 1]);
        j = m;
      }
      cur = tmp;
    }
    return cur;
  }
};
}  // namespace

TEST(Leetcode, count_and_say) {
  Solution s;
  EXPECT_EQ("1", s.countAndSay(1));
  EXPECT_EQ("11", s.countAndSay(2));
  EXPECT_EQ("13211311123113112211", s.countAndSay(10));
}
