#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  std::vector<int> plusOne(std::vector<int>& digits) {
    int size = digits.size();
    int extra = 1;
    for (int i = size - 1; i >= 0; --i) {
      int tmp = digits[i] + extra;
      digits[i] = tmp % 10;
      extra = tmp / 10;
      if (extra == 0) {
        break;
      }
    }
    if (extra > 0) {
      digits.insert(digits.begin(), extra);
    }
    return digits;
  }
};
}  // namespace

TEST(Leetcode, plus_one) {
  Solution s;
  {
    std::vector<int> inputs = {1, 2, 3};
    std::vector<int> exps = {1, 2, 4};
    EXPECT_EQ(exps, s.plusOne(inputs));
  }

  {
    std::vector<int> inputs = {9, 9, 9};
    std::vector<int> exps = {1, 0, 0, 0};
    EXPECT_EQ(exps, s.plusOne(inputs));
  }
}
