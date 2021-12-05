#include "includes/tree.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace
{
class Solution
{
public:
  using TreeNode = leetcode::tree::TreeNode;
  int kthLargest(TreeNode* root, int k)
  {
    int ret;
    visit(&ret, root, k);
    return ret;
  }

private:
  void visit(int* ret, TreeNode* node, int& k)
  {
    if (!node || k == 0) { return; }
    visit(ret, node->right, k);
    if (k == 0) { return; }
    if (--k == 0) { *ret = node->val; }
    visit(ret, node->left, k);
  }
};
}  // namespace

TEST(Leetcode, er_cha_sou_suo_shu_de_di_kda_jie_dian_lcof)
{
  Solution s;
  {
    std::vector<std::string> nodes = {"3", "1", "4", "null", "2"};
    auto root = leetcode::tree::create(nodes);
    EXPECT_EQ(4, s.kthLargest(root, 1));
    leetcode::tree::destroy(root);
  }

  {
    std::vector<std::string> nodes = {"5", "3", "6", "2", "4", "null", "null", "1"};

    auto root = leetcode::tree::create(nodes);
    leetcode::tree::print(root, leetcode::tree::Order::IN_ORDER);
    EXPECT_EQ(4, s.kthLargest(root, 3));
    leetcode::tree::destroy(root);
  }
}
