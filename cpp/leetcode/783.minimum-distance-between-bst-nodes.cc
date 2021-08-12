#include <vector>
#include <string>
#include <climits>
#include <gtest/gtest.h>
#include "includes/tree.h"

namespace {
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;
  int minDiffInBST(TreeNode* root)
  {
    int ret = INT_MAX, pre = -1;
    dfs(ret, pre, root);
    return ret;
  }

private:
  void dfs(int& ret, int& pre, TreeNode* node)
  {
    if (!node) { return; }
    dfs(ret, pre, node->left);
    if (pre >= 0) { ret = std::min(ret, node->val - pre); }
    pre = node->val;
    dfs(ret, pre, node->right);
  }
};
}   // namespace

TEST(Leetcode, minimum_distance_between_bst_nodes)
{
  Solution s;
  {
    std::vector<std::string> nodes = {
        "27",
        "null",
        "34",
        "null",
        "58",
        "50",
        "null",
        "44",
    };
    auto root = leetcode::tree::create(nodes);
    int ret = s.minDiffInBST(root);
    EXPECT_EQ(6, ret);
    leetcode::tree::destroy(root);
  }
}
