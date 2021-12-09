#include <gtest/gtest.h>

#include <functional>
#include <queue>
#include <vector>

namespace {
struct TreeNode {
  int val;
  TreeNode* left;
  TreeNode* right;
  TreeNode(int x) : val(x), left(NULL), right(NULL) {}
  TreeNode(int x, TreeNode* left, TreeNode* right)
      : val(x), left(left), right(right) {}
};

class Solution {
 public:
  std::vector<int> levelOrder(TreeNode* root) {
    if (!root) {
      return {};
    }
    std::vector<int> ret;
    std::queue<TreeNode*> queue;
    queue.push(root);
    while (!queue.empty()) {
      TreeNode* front = queue.front();
      ret.push_back(front->val);
      queue.pop();
      if (front->left) {
        queue.push(front->left);
      }
      if (front->right) {
        queue.push(front->right);
      }
    }
    return ret;
  }
};
}  // namespace

TEST(Leetcode, cong_shang_dao_xia_da_yin_er_cha_shu_lcof) {
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

  std::vector<int> exp = {3, 9, 20, 15, 7};
  TreeNode* root = new TreeNode(
      3, new TreeNode(9), new TreeNode(20, new TreeNode(15), new TreeNode(7)));
  EXPECT_EQ(exp, s.levelOrder(root));

  destroy(root);
}
