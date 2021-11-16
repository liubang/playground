#include "includes/tree.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {
class Solution
{
public:
    using TreeNode = leetcode::tree::TreeNode;
    std::vector<std::string> binaryTreePaths(TreeNode* root)
    {
        std::vector<std::string> ret;
        std::string              cur;
        dfs(&ret, cur, root);
        return ret;
    }

private:
    void dfs(std::vector<std::string>* ret, std::string& cur, TreeNode* node)
    {
        cur.append(std::to_string(node->val));
        int size = cur.length();
        if (!node->left && !node->right) {
            ret->push_back(cur);
            return;
        }

        if (node->left) {
            cur.append("->");
            dfs(ret, cur, node->left);
            cur.erase(cur.begin() + size, cur.end());
        }
        if (node->right) {
            cur.append("->");
            dfs(ret, cur, node->right);
            cur.erase(cur.begin() + size, cur.end());
        }
    }
};
}  // namespace

TEST(Leetcode, binary_tree_paths)
{
    Solution s;
    {
        auto                     root = leetcode::tree::create({"11", "22", "23", "null", "59"});
        std::vector<std::string> exp  = {"11->22->59", "11->23"};
        EXPECT_EQ(exp, s.binaryTreePaths(root));
        leetcode::tree::destroy(root);
    }
}
