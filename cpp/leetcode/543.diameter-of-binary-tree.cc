#include "includes/tree.h"
#include <functional>
#include <gtest/gtest.h>

namespace
{
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;
  int diameterOfBinaryTree(TreeNode* root)
  {
    int ret = 0;
    std::function<int(TreeNode*)> depth = [&](TreeNode* node) -> int {
      if (!node) { return -1; }
      int left = depth(node->left) + 1;
      int right = depth(node->right) + 1;
      ret = std::max(ret, left + right);
      return std::max(left, right);
    };
    depth(root);
    return ret;
  }
};
}  // namespace

TEST(Leetcode, diameter_of_binary_tree)
{
  using TreeNode = leetcode::tree::TreeNode;
  Solution s;
  {
    std::vector<std::string> nodes = {"1", "2", "3", "4", "5"};
    TreeNode* root = leetcode::tree::create(nodes);
    EXPECT_EQ(3, s.diameterOfBinaryTree(root));
    leetcode::tree::destroy(root);
  }

  {
    std::vector<std::string> nodes = {"4",    "-7", "-3",   "null", "null", "-9", "-3",   "9",
                                      "-7",   "-4", "null", "6",    "null", "-6", "-6",   "null",
                                      "null", "0",  "6",    "5",    "null", "9",  "null", "null",
                                      "-1",   "-4", "null", "null", "null", "-2"};
    TreeNode* root = leetcode::tree::create(nodes);
    EXPECT_EQ(8, s.diameterOfBinaryTree(root));
    leetcode::tree::destroy(root);
  }
}
