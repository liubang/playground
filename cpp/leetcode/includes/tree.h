#pragma once

namespace leetcode {
namespace tree {
struct TreeNode {
  int val;
  TreeNode* left;
  TreeNode* right;
  TreeNode(int x, TreeNode* left = nullptr, TreeNode* right = nullptr)
      : val(x), left(left), right(right) {}
};

void destroy(TreeNode* node) {
  if (!node) {
    return;
  }
  destroy(node->left);
  destroy(node->right);
  delete node;
}
} // namespace tree
} // namespace leetcode
