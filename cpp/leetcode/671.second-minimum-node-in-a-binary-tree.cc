#include <gtest/gtest.h>
#include <vector>

#include "includes/tree.h"

namespace
{
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;
  int findSecondMinimumValue(TreeNode* root) { return dfs(root, root->val); }

private:
  int dfs(TreeNode* root, int val)
  {
    if (!root) { return -1; }
    if (root->val > val) { return root->val; }
    int left = dfs(root->left, val);
    int right = dfs(root->right, val);
    if (left < 0) { return right; }
    if (right < 0) { return left; }
    return std::min(left, right);
  }
};
}  // namespace

TEST(Leetcode, second_minimum_node_in_a_binary_tree)
{
  Solution s;
  auto root = leetcode::tree::create({"2", "2", "5", "null", "null", "5", "7"});
  EXPECT_EQ(5, s.findSecondMinimumValue(root));
  leetcode::tree::destroy(root);
}
