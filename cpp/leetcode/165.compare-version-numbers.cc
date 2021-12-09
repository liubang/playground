#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  int compareVersion(const std::string& version1, const std::string& version2) {
    int i = 0, j = 0;
    int l1 = version1.size(), l2 = version2.size();
    while (i < l1 || j < l2) {
      int v1 = 0;
      while (i < l1 && version1[i] != '.') {
        v1 = v1 * 10 + (version1[i++] - '0');
      }
      int v2 = 0;
      while (j < l2 && version2[j] != '.') {
        v2 = v2 * 10 + (version2[j++] - '0');
      }
      if (v1 != v2) {
        return v1 > v2 ? 1 : -1;
      }
      i++;
      j++;
    }
    return 0;
  }
};
}  // namespace

TEST(Leetcode, compare_version_numbers) {
  Solution s;
  EXPECT_EQ(0, s.compareVersion("1.01", "1.001"));
  EXPECT_EQ(-1, s.compareVersion("0.1", "1.0"));
  EXPECT_EQ(1, s.compareVersion("1.1.01", "1.0.01"));
}
