#include <gtest/gtest.h>
#include <vector>

#include "includes/tree.h"

namespace {
using TreeNode = leetcode::tree::TreeNode;
class Solution
{
public:
    int maxValue(TreeNode* root, int k)
    {
        auto f = dfs(root, k);
        return *std::max_element(f.begin(), f.end());
    }

private:
    std::vector<int> dfs(TreeNode* root, int k)
    {
        std::vector<int> dp(k + 1, 0);
        if (!root)
            return dp;
        auto l = dfs(root->left, k);
        auto r = dfs(root->right, k);
        dp[0]  = *std::max_element(l.begin(), l.end()) + *std::max_element(r.begin(), r.end());
        for (int i = 1; i <= k; ++i) {
            for (int j = 0; j < i; ++j) {
                dp[i] = std::max(dp[i], root->val + l[j] + r[i - 1 - j]);
            }
        }
        return dp;
    }
};
}  // namespace

TEST(Leetcode, er_cha_shu_ran_se_UGC)
{
    Solution s;
    {
        auto root = leetcode::tree::create({"5", "2", "3", "4"});
        EXPECT_EQ(12, s.maxValue(root, 2));
        leetcode::tree::destroy(root);
    }

    {
        auto root = leetcode::tree::create({"4", "1", "3", "9", "null", "null", "2"});
        EXPECT_EQ(16, s.maxValue(root, 2));
        leetcode::tree::destroy(root);
    }
}
