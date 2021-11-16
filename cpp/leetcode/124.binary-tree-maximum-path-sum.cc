#include "includes/tree.h"
#include <climits>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {
class Solution
{
public:
    int maxPathSum(leetcode::tree::TreeNode* root)
    {
        int                                           ret    = INT_MIN;
        std::function<int(leetcode::tree::TreeNode*)> getMax = [&](leetcode::tree::TreeNode* node) {
            if (!node) {
                return 0;
            }
            int left  = std::max(0, getMax(node->left));
            int right = std::max(0, getMax(node->right));
            ret       = std::max(ret, node->val + left + right);
            return std::max(left, right) + node->val;
        };
        getMax(root);
        return ret;
    }
};
}  // namespace

TEST(Leetcode, binary_tree_maximun_path_sum)
{
    Solution                  s;
    std::vector<std::string>  nodes = {"-10", "9", "20", "null", "null", "15", "7"};
    leetcode::tree::TreeNode* root  = leetcode::tree::create(nodes);
    EXPECT_EQ(42, s.maxPathSum(root));
    leetcode::tree::destroy(root);
}
