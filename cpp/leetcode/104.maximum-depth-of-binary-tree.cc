#include "includes/tree.h"
#include <gtest/gtest.h>

namespace {
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;
  int maxDepth(TreeNode* root)
  {
    return root == nullptr ? 0 : std::max(maxDepth(root->left), maxDepth(root->right)) + 1;
  }
};
}   // namespace

TEST(Leetcode, maximum_depth_of_binary_tree)
{
  using TreeNode = leetcode::tree::TreeNode;
  Solution s;
  TreeNode* root =
      new TreeNode(3, new TreeNode(9), new TreeNode(20, new TreeNode(15), new TreeNode(7)));

  EXPECT_EQ(3, s.maxDepth(root));
  leetcode::tree::destroy(root);
}
