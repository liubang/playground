#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  // 基本思路是先找到第一个元素，然后就非常简单了
  std::vector<int> decode(const std::vector<int>& encoded) {
    int size = encoded.size();
    int a = 0;
    for (int i = 1; i <= size + 1; ++i) {
      a ^= i;
    }
    for (int i = 1; i < size; i += 2) {
      a ^= encoded[i];
    }
    std::vector<int> ret;
    ret.push_back(a);
    for (int i = 0; i < size; ++i) {
      a ^= encoded[i];
      ret.push_back(a);
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, decode_xored_permutation) {
  Solution s;
  {
    std::vector<int> exp = {1, 2, 3};
    EXPECT_EQ(exp, s.decode({3, 1}));
  }

  {
    std::vector<int> exp = {2, 4, 1, 5, 3};
    EXPECT_EQ(exp, s.decode({6, 5, 4, 6}));
  }
}
