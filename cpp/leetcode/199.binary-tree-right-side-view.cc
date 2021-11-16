#include "includes/tree.h"
#include <gtest/gtest.h>
#include <queue>
#include <vector>

namespace {
class Solution
{
public:
    using TreeNode = leetcode::tree::TreeNode;

    std::vector<int> rightSideView(TreeNode* root)
    {
        if (!root) {
            return {};
        }
        std::queue<TreeNode*> queue;
        std::vector<int>      ret;
        queue.push(root);
        while (!queue.empty()) {
            int size = queue.size();
            for (int i = 0; i < size; ++i) {
                TreeNode* front = queue.front();
                if (i == size - 1) {
                    ret.push_back(front->val);
                }
                queue.pop();
                if (front->left) {
                    queue.push(front->left);
                }
                if (front->right) {
                    queue.push(front->right);
                }
            }
        }
        return ret;
    }
};
}  // namespace

TEST(Leetcode, binary_tree_right_side_view)
{
    using TreeNode = leetcode::tree::TreeNode;
    Solution s;
    {
        TreeNode* root = new TreeNode(1,
                                      new TreeNode(2, nullptr, new TreeNode(5)),
                                      new TreeNode(3, nullptr, new TreeNode(4)));

        std::vector<int> exp = {1, 3, 4};
        EXPECT_EQ(exp, s.rightSideView(root));
        leetcode::tree::destroy(root);
    }
}
