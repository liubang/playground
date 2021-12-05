#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "includes/tree.h"

namespace {
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;
  bool isSymmetric(TreeNode* root)
  {
    if (!root) return false;
    return ismirror(root->left, root->right);
  }

private:
  bool ismirror(TreeNode* l, TreeNode* r)
  {
    if (!l && !r) return true;
    if ((!l || !r) || (l->val != r->val)) return false;
    return ismirror(l->left, r->right) && ismirror(l->right, r->left);
  }
};
}  // namespace

TEST(Leetcode, symmetric_tree)
{
  Solution s;
  {
    std::vector<std::string> nodes = {"1", "2", "2", "3", "4", "4", "3"};
    auto root = leetcode::tree::create(nodes);
    EXPECT_TRUE(s.isSymmetric(root));
    leetcode::tree::destroy(root);
  }

  {
    std::vector<std::string> nodes = {"1", "2", "2", "null", "3", "null", "3"};
    auto root = leetcode::tree::create(nodes);
    EXPECT_FALSE(s.isSymmetric(root));
    leetcode::tree::destroy(root);
  }
}
