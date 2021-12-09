#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

namespace {
class Solution {
 public:
  int lengthOfLongestSubstring(const std::string& s) {
    if (s.empty()) {
      return 0;
    }
    int pre = 1, ret = 1, len = s.length();
    std::unordered_map<char, int> map;
    map[s[0]] = 0;
    for (int i = 1; i < len; ++i) {
      if (map.find(s[i]) != map.end() && (i - pre) <= map[s[i]]) {
        pre = i - map[s[i]];
      } else {
        pre++;
      }
      ret = std::max(ret, pre);
      map[s[i]] = i;
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, longest_substring_without_repeating_characters) {
  Solution s;
  EXPECT_EQ(3, s.lengthOfLongestSubstring("abcabcbb"));
  EXPECT_EQ(1, s.lengthOfLongestSubstring("bbbbbbbb"));
  EXPECT_EQ(26,
            s.lengthOfLongestSubstring("abcdadbdqwertyuioplkjhgfdsazxcvbnm"));
}
