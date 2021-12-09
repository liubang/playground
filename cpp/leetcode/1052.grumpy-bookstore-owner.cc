#include <gtest/gtest.h>

#include <vector>

namespace {
class Solution {
 public:
  int maxSatisfied(std::vector<int>& customers, std::vector<int>& grumpy,
                   int X) {
    int ret = 0, cur = 0;
    size_t len = customers.size();
    for (auto i = 0; i < len; ++i) {
      if (i < X || grumpy[i] == 0) cur += customers[i];
    }
    ret = cur;
    for (auto i = X; i < len; ++i) {
      if (grumpy[i] == 1) cur += customers[i];
      if (grumpy[i - X] == 1) cur -= customers[i - X];
      ret = std::max(ret, cur);
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, grumpy_bookstore_owner) {
  Solution s;
  {
    std::vector<int> customers = {1, 0, 1, 2, 1, 1, 7, 5};
    std::vector<int> grumpy = {0, 1, 0, 1, 0, 1, 0, 1};
    auto ret = s.maxSatisfied(customers, grumpy, 3);
    EXPECT_EQ(16, ret);
  }
  {
    std::vector<int> customers = {4, 10, 10};
    std::vector<int> grumpy = {1, 1, 0};
    auto ret = s.maxSatisfied(customers, grumpy, 2);
    EXPECT_EQ(24, ret);
  }
}
