#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {
class Solution {
 public:
  std::string largestNumber(const std::vector<int>& nums) {
    if (nums.empty()) {
      return "";
    }
    std::vector<std::string> strs;
    for (auto num : nums) {
      strs.push_back(std::to_string(num));
    }
    std::sort(strs.begin(), strs.end(),
              [](auto& a, auto& b) { return (a + b) > (b + a); });
    std::string ret;
    for (auto& str : strs) {
      ret.append(str);
    }
    if (ret[0] == '0') {
      return "0";
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, largest_number) {
  Solution s;
  EXPECT_EQ("210", s.largestNumber({10, 2}));
  EXPECT_EQ("9534330", s.largestNumber({3, 30, 34, 5, 9}));
  EXPECT_EQ("343234323", s.largestNumber({34323, 3432}));
}
