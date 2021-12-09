#include <gtest/gtest.h>

#include <functional>
#include <queue>
#include <vector>

namespace {
struct TreeNode {
  int val;
  TreeNode* left;
  TreeNode* right;
  TreeNode(int x) : val(x), left(nullptr), right(nullptr) {}
  TreeNode(int x, TreeNode* left, TreeNode* right)
      : val(x), left(left), right(right) {}
};

class Solution {
 public:
  std::vector<std::vector<int>> levelOrder(TreeNode* root) {
    if (!root) {
      return {};
    }
    std::vector<std::vector<int>> ret;
    std::queue<TreeNode*> queue;
    queue.push(root);
    while (!queue.empty()) {
      int size = queue.size();
      std::vector<int> row;
      for (int i = 0; i < size; ++i) {
        TreeNode* front = queue.front();
        queue.pop();
        row.push_back(front->val);
        if (front->left) {
          queue.push(front->left);
        }
        if (front->right) {
          queue.push(front->right);
        }
      }
      ret.emplace_back(std::move(row));
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, binary_tree_level_order_traversal) {
  Solution s;
  using DestroyType = std::function<void(TreeNode*)>;
  DestroyType destroy = [&](TreeNode* node) {
    if (!node) {
      return;
    }
    destroy(node->left);
    destroy(node->right);
    delete node;
  };

  TreeNode* root = new TreeNode(
      3, new TreeNode(9), new TreeNode(20, new TreeNode(15), new TreeNode(7)));
  std::vector<std::vector<int>> exp = {{3}, {9, 20}, {15, 7}};
  auto ret = s.levelOrder(root);
  EXPECT_EQ(exp, ret);

  destroy(root);
}
