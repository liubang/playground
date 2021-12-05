#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "includes/tree.h"

namespace {
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;
  int sumNumbers(TreeNode* root) { return dfs(root, 0); }

private:
  int dfs(TreeNode* node, int sum)
  {
    if (!node) { return 0; }
    sum = sum * 10 + node->val;
    if (!node->left && !node->right) { return sum; }
    return dfs(node->left, sum) + dfs(node->right, sum);
  }
};
}  // namespace

TEST(Leetcode, sum_root_to_leaf_numbers)
{
  Solution s;
  {
    std::vector<std::string> nodes = {"1", "2", "3"};
    auto root = leetcode::tree::create(nodes);
    EXPECT_EQ(25, s.sumNumbers(root));
    leetcode::tree::destroy(root);
  }

  {
    std::vector<std::string> nodes = {"4", "9", "0", "5", "1"};
    auto root = leetcode::tree::create(nodes);
    EXPECT_EQ(1026, s.sumNumbers(root));
    leetcode::tree::destroy(root);
  }
}
