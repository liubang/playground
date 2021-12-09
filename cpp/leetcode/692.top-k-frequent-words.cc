#include <gtest/gtest.h>

#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
class Solution {
 public:
  std::vector<std::string> topKFrequent(const std::vector<std::string>& words,
                                        int k) {
    std::unordered_map<std::string, int> mmp;
    for (const auto& word : words) {
      mmp[word]++;
    }
    auto my_compare = [](const std::pair<std::string, int>& p1,
                         const std::pair<std::string, int>& p2) {
      if (p1.second == p2.second) {
        return p1.first < p2.first;
      }
      return p1.second > p2.second;
    };

    std::priority_queue<std::pair<std::string, int>,
                        std::vector<std::pair<std::string, int>>,
                        decltype(my_compare)>
        queue(my_compare);

    for (auto& pair : mmp) {
      if (queue.size() < k) {
        queue.push(pair);
      } else {
        auto top = queue.top();
        if ((top.second == pair.second && top.first > pair.first) ||
            (top.first < pair.first)) {
          queue.pop();
          queue.push(pair);
        }
      }
    }
    std::vector<std::string> ret;
    while (!queue.empty()) {
      ret.insert(ret.begin(), queue.top().first);
      queue.pop();
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, top_k_frequent_words) {
  Solution s;
  {
    std::vector<std::string> exp = {"i", "love"};
    EXPECT_EQ(exp, s.topKFrequent(
                       {"i", "love", "leetcode", "i", "love", "coding"}, 2));
  }

  {
    std::vector<std::string> exp = {"the", "is", "sunny", "day"};
    EXPECT_EQ(exp, s.topKFrequent({"the", "day", "is", "sunny", "the", "the",
                                   "the", "sunny", "is", "is"},
                                  4));
  }
}
