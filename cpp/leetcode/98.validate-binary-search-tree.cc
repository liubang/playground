#include "includes/tree.h"
#include <climits>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace
{
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;

  bool isValidBST(TreeNode* root) { return validate1(root, INT_MIN, INT_MAX); }

  bool isValidBST2(TreeNode* root)
  {
    std::vector<int> vecs;
    return validate2(root, vecs);
  }

private:
  bool validate1(TreeNode* node, int lower, int upper)
  {
    if (!node) { return true; }
    if (node->val <= lower || node->val >= upper) { return false; }
    return validate1(node->left, lower, node->val) && validate1(node->right, node->val, upper);
  }

  bool validate2(TreeNode* node, std::vector<int>& vecs)
  {
    if (!node) { return true; }
    bool ret = validate2(node->left, vecs);
    if (!ret) { return false; }
    if (!vecs.empty() && vecs[vecs.size() - 1] >= node->val) { return false; }
    vecs.push_back(node->val);
    return validate2(node->right, vecs);
  }
};
}  // namespace

TEST(Leetcode, validate_binary_search_tree)
{
  using TreeNode = leetcode::tree::TreeNode;
  Solution s;
  {
    std::vector<std::string> nodes = {"2", "1", "3"};
    TreeNode* root = leetcode::tree::create(nodes);

    EXPECT_TRUE(s.isValidBST(root));
    EXPECT_TRUE(s.isValidBST2(root));

    leetcode::tree::destroy(root);
  }

  {
    std::vector<std::string> nodes = {"5", "4", "6", "null", "null", "3", "7"};
    TreeNode* root = leetcode::tree::create(nodes);
    EXPECT_FALSE(s.isValidBST(root));
    EXPECT_FALSE(s.isValidBST2(root));
    leetcode::tree::destroy(root);
  }
}
