#include <vector>
#include <string>
#include <queue>
#include <gtest/gtest.h>

#include "includes/tree.h"

namespace {
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;
  TreeNode* invertTree(TreeNode* root)
  {
    if (!root) return root;
    std::queue<TreeNode*> queue;
    queue.push(root);
    while (!queue.empty()) {
      TreeNode* cur = queue.front();
      queue.pop();
      std::swap(cur->left, cur->right);
      if (cur->left) queue.push(cur->left);
      if (cur->right) queue.push(cur->right);
    }
    return root;
  }
};
}  // namespace

TEST(Leetcode, invert_binary_tree)
{
  Solution s;
  {
    std::vector<std::string> nodes = {"4", "2", "7", "1", "3", "6", "9"};
    std::vector<std::string> expnodes = {"4", "7", "2", "9", "6", "3", "1"};
    auto root = leetcode::tree::create(nodes);
    auto exp = leetcode::tree::create(expnodes);
    auto ret = s.invertTree(root);
    EXPECT_TRUE(leetcode::tree::equals(exp, ret));
    leetcode::tree::destroy(exp);
    leetcode::tree::destroy(ret);
  }
}
