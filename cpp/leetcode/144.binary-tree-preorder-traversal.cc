#include "includes/tree.h"
#include <gtest/gtest.h>
#include <vector>

namespace
{
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;
  std::vector<int> preorderTraversal(TreeNode* root)
  {
    std::vector<int> ret;
    visit(&ret, root);
    return ret;
  }

private:
  void visit(std::vector<int>* ret, TreeNode* node)
  {
    if (!node) { return; }
    ret->push_back(node->val);
    visit(ret, node->left);
    visit(ret, node->right);
  }
};
}  // namespace

TEST(Leetcode, binary_tree_preorder_traversal)
{
  using TreeNode = leetcode::tree::TreeNode;
  Solution s;

  {
    TreeNode* root = new TreeNode(1, nullptr, new TreeNode(2, new TreeNode(3)));
    std::vector<int> exp = {1, 2, 3};
    EXPECT_EQ(exp, s.preorderTraversal(root));
    leetcode::tree::destroy(root);
  }

  {
    TreeNode* root = new TreeNode(1, new TreeNode(2));
    std::vector<int> exp = {1, 2};
    EXPECT_EQ(exp, s.preorderTraversal(root));
    leetcode::tree::destroy(root);
  }

  {
    TreeNode* root = new TreeNode(1, nullptr, new TreeNode(2));
    std::vector<int> exp = {1, 2};
    EXPECT_EQ(exp, s.preorderTraversal(root));
    leetcode::tree::destroy(root);
  }
}
