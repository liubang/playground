#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace {
class Solution {
 public:
  std::string addBinary(const std::string& a, const std::string& b) {
    int i = a.length() - 1, j = b.length() - 1;
    std::string ret;
    int extra = 0;
    while (i >= 0 || j >= 0) {
      int tmp = extra;
      if (i >= 0) {
        tmp += a[i--] - '0';
      }
      if (j >= 0) {
        tmp += b[j--] - '0';
      }
      ret.insert(static_cast<size_t>(0), 1, (tmp % 2) + '0');
      extra = tmp / 2;
    }
    if (extra > 0) {
      ret.insert(static_cast<size_t>(0), 1, extra + '0');
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, add_binary) {
  Solution s;
  EXPECT_EQ("11110", s.addBinary("1111", "1111"));
}
