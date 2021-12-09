#include <gtest/gtest.h>

#include <unordered_map>
#include <vector>

namespace {
class Solution {
 public:
  std::vector<double> dicesProbability(int n) {
    std::vector<double> ret;
    for (int i = n; i <= 6 * n; ++i) {
      ret.push_back(f(n, i));
    }
    return ret;
  }

 private:
  double f(int n, int x) {
    if (n == 1) {
      return 1.0 / 6.0;
    }
    double ret = 0.0;
    for (int i = 1; i <= 6; ++i) {
      int new_n = n - 1;
      int new_x = x - i;
      if (new_x < new_n || new_x > 6 * new_n) {
        continue;
      }
      if (dp_.find(new_n) != dp_.end()) {
        auto mmp = dp_[new_n];
        if (mmp.find(new_x) != mmp.end()) {
          ret += 1.0 / 6.0 * mmp[new_x];
          continue;
        }
      }
      double tmp = f(new_n, new_x);
      dp_[new_n][new_x] = tmp;
      ret += 1.0 / 6.0 * tmp;
    }
    return ret;
  }

 private:
  std::unordered_map<int, std::unordered_map<int, double>> dp_;
};
}  // namespace

TEST(Leetcode, nge_tou_zi_de_dian_shu) {
  Solution s;
  {
    std::vector<double> exp = {0.16667, 0.16667, 0.16667,
                               0.16667, 0.16667, 0.16667};
    auto ret = s.dicesProbability(1);
    EXPECT_EQ(exp.size(), ret.size());
    for (int i = 0; i < exp.size(); ++i) {
      EXPECT_LE(ret[i] - exp[i], 0.00001);
    }
  }

  {
    std::vector<double> exp = {0.02778, 0.05556, 0.08333, 0.11111,
                               0.13889, 0.16667, 0.13889, 0.11111,
                               0.08333, 0.05556, 0.02778};
    auto ret = s.dicesProbability(2);
    EXPECT_EQ(exp.size(), ret.size());
    for (int i = 0; i < exp.size(); ++i) {
      EXPECT_LE(ret[i] - exp[i], 0.00001);
    }
  }

  {
    std::vector<double> exp = {
        0.00013, 0.00064, 0.00193, 0.00450, 0.00900, 0.01620, 0.02636,
        0.03922, 0.05401, 0.06944, 0.08372, 0.09452, 0.10031, 0.10031,
        0.09452, 0.08372, 0.06944, 0.05401, 0.03922, 0.02636, 0.01620,
        0.00900, 0.00450, 0.00193, 0.00064, 0.00013};
    auto ret = s.dicesProbability(5);
    EXPECT_EQ(exp.size(), ret.size());
    for (int i = 0; i < exp.size(); ++i) {
      EXPECT_LE(ret[i] - exp[i], 0.00001);
    }
  }

  {
    std::vector<double> exp = {
        0.00000, 0.00000, 0.00000, 0.00000, 0.00000, 0.00001, 0.00002, 0.00005,
        0.00012, 0.00025, 0.00048, 0.00088, 0.00154, 0.00257, 0.00409, 0.00625,
        0.00919, 0.01301, 0.01778, 0.02347, 0.02995, 0.03702, 0.04432, 0.05145,
        0.05793, 0.06331, 0.06715, 0.06916, 0.06916, 0.06715, 0.06331, 0.05793,
        0.05145, 0.04432, 0.03702, 0.02995, 0.02347, 0.01778, 0.01301, 0.00919,
        0.00625, 0.00409, 0.00257, 0.00154, 0.00088, 0.00048, 0.00025, 0.00012,
        0.00005, 0.00002, 0.00001, 0.00000, 0.00000, 0.00000, 0.00000, 0.00000};
    auto ret = s.dicesProbability(11);
    EXPECT_EQ(exp.size(), ret.size());
    for (int i = 0; i < exp.size(); ++i) {
      EXPECT_LE(ret[i] - exp[i], 0.00001);
    }
  }
}
