/**
 * 1. 二分法求方程的根：
 *    f(x) = X^3 - 5x + 10x - 80 = 0
 *    用二分法计算方程的一个根，并精确到10^-6
 *
 * 2. 输入n(n <= 100000)个整数，找出其中两个数，他们之
 *    和等于整数m(假定肯定有解)。
 */
#include <gtest/gtest.h>
#include <utility>

namespace {
constexpr double EPS = 1e-6;
class EquationRoot {
 public:
  double getRoot() {
    double s = 0, e = 100;
    double m = (s + e) / 2;
    double y = f(m);
    while (std::abs(y) > EPS) {
      if (y > 0) {
        e = m;
      } else {
        s = m;
      }
      m = (s + e) / 2;
      y = f(m);
    }
    return m;
  }

 private:
  inline double f(double x) {
    return x * x * x - 5 * x * x + 10 * x - 80;
  }
};

class TowSum {
 public:
  std::pair<int, int> find(std::vector<int>& nums, int target) {
    std::sort(nums.begin(), nums.end());
    int i = 0, j = nums.size() - 1;
    while (i <= j) {
      int sum = nums[i] + nums[j];
      if (sum == target) {
        break;
      } else if (target > sum) {
        i++;
      } else {
        j--;
      }
    }
    return std::make_pair(nums[i], nums[j]);
  }
};
} // namespace

TEST(algo, EquationRoot) {
  EquationRoot e;
  double root = e.getRoot();
  EXPECT_NEAR(root, 5.70508593, 1e-6);
}

TEST(algo, TowSum) {
  TowSum t;
  std::vector<int> inputs = {1, 2, 8, 4, 3, 6, 13};
  std::pair<int, int> exp(4, 8);
  EXPECT_EQ(t.find(inputs, 12), exp);
}
