#include <gtest/gtest.h>

#include <queue>
#include <string>
#include <vector>

#include "includes/tree.h"

namespace {
class Solution {
 public:
  using TreeNode = leetcode::tree::TreeNode;
  bool isCompleteTree(TreeNode* root) {
    // 二叉树层次遍历
    std::queue<TreeNode*> queue;
    queue.push(root);
    int h = 1;
    while (!queue.empty()) {
      int size = queue.size();
      // 有中断
      int flag = true;
      for (int i = 0; i < size; ++i) {
        TreeNode* front = queue.front();
        queue.pop();
        if (front->left) {
          if (!flag) return false;
          queue.push(front->left);
        } else {
          flag = false;
        }

        if (front->right) {
          if (!flag) return false;
          queue.push(front->right);
        } else {
          flag = false;
        }
      }
      if (!queue.empty() && size != (1 << (h - 1))) {
        return false;
      }
      h++;
    }
    return true;
  }
};
}  // namespace

TEST(Leetcode, check_completeness_of_a_binary_tree) {
  Solution s;
  {
    std::vector<std::string> nodes = {"1", "2", "3", "4", "5", "6"};
    auto root = leetcode::tree::create(nodes);
    EXPECT_TRUE(s.isCompleteTree(root));
    leetcode::tree::destroy(root);
  }

  {
    std::vector<std::string> nodes = {"1", "2", "3", "4", "5", "null", "7"};
    auto root = leetcode::tree::create(nodes);
    EXPECT_FALSE(s.isCompleteTree(root));
    leetcode::tree::destroy(root);
  }
}
