#include "includes/tree.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {
class Solution {
public:
  using TreeNode = leetcode::tree::TreeNode;
  bool hasPathSum(TreeNode *root, int target) { return dfs(root, 0, target); }

private:
  bool dfs(TreeNode *node, int sum, int target) {
    if (!node) {
      return false;
    }
    sum += node->val;
    if (!node->left && !node->right) {
      return sum == target;
    }
    return dfs(node->left, sum, target) || dfs(node->right, sum, target);
  }
};
} // namespace

TEST(Leetcode, path_sum) {
  Solution s;
  {
    std::vector<std::string> nodes = {"5",    "4",    "8", "11", "null",
                                      "13",   "4",    "7", "2",  "null",
                                      "null", "null", "1"};
    auto root = leetcode::tree::create(nodes);
    EXPECT_TRUE(s.hasPathSum(root, 22));
    EXPECT_FALSE(s.hasPathSum(root, 11));
    leetcode::tree::destroy(root);
  }
}
