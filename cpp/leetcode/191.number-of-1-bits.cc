#include <gtest/gtest.h>

namespace {
class Solution {
 public:
  int hammingWeight(uint32_t n) {
    int count = 0;
    while (n > 0) {
      if ((n & 1) == 1) {
        count++;
      }
      n = n >> 1;
    }
    return count;
  }
};
}  // namespace

TEST(Leetcode, number_of_1_bits) {
  Solution s;
  EXPECT_EQ(3, s.hammingWeight(0b00000000000000000000000000001011));
  EXPECT_EQ(31, s.hammingWeight(0b11111111111111111111111111111101));
  EXPECT_EQ(1, s.hammingWeight(0b00000000000000000000000010000000));
}
