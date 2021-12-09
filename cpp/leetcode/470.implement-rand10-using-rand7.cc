#include <gtest/gtest.h>
#include <time.h>

#include <random>
#include <unordered_map>

namespace {

class Solution {
 public:
  Solution() { e_.seed(time(0)); }

  int rand10() {
    int ret = 0;
    for (;;) {
      ret = (rand7() - 1) * 7 + rand7();
      if (ret <= 40) {
        break;
      }
    }
    return ret % 10 + 1;
  }

 private:
  int rand7() { return u_(e_); }

 private:
  std::default_random_engine e_;
  std::uniform_int_distribution<int> u_{1, 7};
};
}  // namespace

TEST(Leetcode, implement_rand10_using_rand7) {
  Solution s;
  EXPECT_GE(s.rand10(), 1);
  EXPECT_LE(s.rand10(), 10);
  std::unordered_map<int, int> map;
  for (int i = 0; i < 10000; ++i) {
    int n = s.rand10();
    map[n]++;
  }

  for (auto& [a, b] : map) {
    EXPECT_GE(a, 1);
    EXPECT_LE(a, 10);
    EXPECT_GE(b, 800);
    EXPECT_LE(b, 1200);
  }
}
