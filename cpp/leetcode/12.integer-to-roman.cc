#include <gtest/gtest.h>

#include <map>

namespace {
class Solution {
 public:
  std::string intToRoman(int num) {
    std::pair<int, std::string> value_symbols[] = {
        {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"},
        {90, "XC"},  {50, "L"},   {40, "XL"}, {10, "X"},   {9, "IX"},
        {5, "V"},    {4, "IV"},   {1, "I"},
    };
    std::string ret;
    for (const auto& [value, symbol] : value_symbols) {
      while (num >= value) {
        num -= value;
        ret += symbol;
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, integer_to_roman) {
  Solution s;
  EXPECT_EQ("MXXIV", s.intToRoman(1024));
  EXPECT_EQ("MMCCXIII", s.intToRoman(2213));
  EXPECT_EQ("MCCXXXIV", s.intToRoman(1234));
}
