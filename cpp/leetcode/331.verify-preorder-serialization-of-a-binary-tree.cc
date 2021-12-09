#include <gtest/gtest.h>

#include <string>

namespace {
class Solution {
 public:
  bool isValidSerialization(const std::string& preorder) {
    if (preorder == "#") return false;
    int len = preorder.length(), inDegree = 0, outDegree = 0;
    for (int i = 0; i < len; ++i) {
      if (preorder[i] == ',') continue;
      if (i == 0) {
        if (preorder[i] == '#') return false;
        outDegree += 2;
      } else {
        if (preorder[i] != '#') {
          outDegree += 2;
        }
        inDegree++;
      }
      if (i != len - 1 && inDegree >= outDegree) {
        return false;
      }
      while ((i + 1 < len && preorder[i + 1] != ',') || (i + 1 == len)) {
        i++;
      }
    }
    return inDegree == outDegree;
  }
};
}  // namespace

TEST(Leetcode, verify_preorder_serialization_of_a_binary_tree) {
  Solution s;

  EXPECT_TRUE(s.isValidSerialization("9,3,4,#,#,1,#,#,2,#,6,#,#"));
  EXPECT_TRUE(s.isValidSerialization("9,#,92,#,#"));
  EXPECT_FALSE(s.isValidSerialization("1,#"));
  EXPECT_FALSE(s.isValidSerialization("9,#,#,1"));
}
