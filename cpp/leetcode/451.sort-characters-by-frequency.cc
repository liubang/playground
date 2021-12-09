#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_map>

namespace {
class Solution {
 public:
  std::string frequencySort(const std::string& s) {
    std::unordered_map<char, int> charcount;
    for (auto& ch : s) {
      charcount[ch]++;
    }
    std::vector<std::pair<char, int>> v(charcount.begin(), charcount.end());
    std::sort(v.begin(), v.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.second > rhs.second;
    });
    std::string ret;
    for (auto& p : v) {
      ret.append(p.second, p.first);
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, sort_characters_by_frequency) {
  Solution s;
  {
    std::string input = "tree";
    auto ret = s.frequencySort(input);
    EXPECT_TRUE(ret == "eetr" || ret == "eert");
  }
  {
    std::string input = "cccaaa";
    auto ret = s.frequencySort(input);
    EXPECT_TRUE(ret == "cccaaa" || ret == "aaaccc");
  }
  {
    std::string input = "Aabb";
    auto ret = s.frequencySort(input);
    EXPECT_TRUE(ret == "bbaA" || ret == "bbAa");
  }
}
