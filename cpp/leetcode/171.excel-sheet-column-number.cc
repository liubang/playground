#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  int titleToNumber(const std::string& columnTitle) {
    int ret = 0;
    for (auto& c : columnTitle) {
      ret = ret * 26 + (c - 'A' + 1);
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, execl_sheet_column_number) {
  Solution s;
  EXPECT_EQ(1, s.titleToNumber("A"));
  EXPECT_EQ(2321206, s.titleToNumber("EBASD"));
}
