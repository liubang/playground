#include <vector>
#include <queue>
#include <gtest/gtest.h>

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
  std::vector<std::vector<int>> levelOrder(TreeNode* root) {
    if (!root) {
      return {};
    }
    std::queue<TreeNode*> queue;
    queue.push(root);
    std::vector<std::vector<int>> ret;
    int i = 0;
    while (!queue.empty()) {
      int size = queue.size();
      std::vector<int> row;
      for (int m = 0; m < size; ++m) {
        TreeNode* front = queue.front();
        queue.pop();
        if ((i & 1) == 0) {
          row.push_back(front->val);
        } else {
          row.insert(row.begin(), front->val);
        }
        if (front->left) {
          queue.push(front->left);
        }
        if (front->right) {
          queue.push(front->right);
        }
      }
      ret.emplace_back(std::move(row));
      i++;
    }
    return ret;
  }
};
} // namespace

TEST(Leetcode, /* name */) {
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
  std::vector<std::vector<int>> exp = {{3}, {20, 9}, {15, 7}};
  EXPECT_EQ(exp, s.levelOrder(root));

  destroy(root);
}
