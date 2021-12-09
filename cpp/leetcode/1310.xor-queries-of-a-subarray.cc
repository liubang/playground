#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> xorQueries(const std::vector<int>& arr,
                              const std::vector<std::vector<int>>& queries) {
    std::vector<int> preprocess;
    int num = 0;
    for (auto a : arr) {
      num ^= a;
      preprocess.push_back(num);
    }
    std::vector<int> ret;
    for (auto& pair : queries) {
      int l = pair[0], r = pair[1];
      if (l == 0) {
        ret.push_back(preprocess[r]);
      } else {
        ret.push_back(preprocess[r] ^ preprocess[l - 1]);
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, xor_queries_of_a_subarray) {
  Solution s;
  {
    std::vector<int> exp = {2, 7, 14, 8};
    EXPECT_EQ(exp,
              s.xorQueries({1, 3, 4, 8}, {{0, 1}, {1, 2}, {0, 3}, {3, 3}}));
  }

  {
    std::vector<int> exp = {8, 0, 4, 4};
    EXPECT_EQ(exp,
              s.xorQueries({4, 8, 2, 10}, {{2, 3}, {1, 3}, {0, 0}, {0, 3}}));
  }
}
