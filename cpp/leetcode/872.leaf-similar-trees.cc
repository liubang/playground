#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "includes/tree.h"

namespace {
class Solution {
 public:
  using TreeNode = leetcode::tree::TreeNode;
  bool leafSimilar(TreeNode* root1, TreeNode* root2) {
    std::vector<int> vecs1;
    std::vector<int> vecs2;
    dfs(vecs1, root1);
    dfs(vecs2, root2);
    return vecs1 == vecs2;
  }

 private:
  void dfs(std::vector<int>& vecs, TreeNode* node) {
    if (!node) {
      return;
    }
    dfs(vecs, node->left);
    if (!node->left && !node->right) {
      vecs.push_back(node->val);
    }
    dfs(vecs, node->right);
  }
};
}  // namespace

TEST(Leetcode, leaf_similar_trees) {
  Solution s;
  {
    auto root1 = leetcode::tree::create({"1", "2"});
    auto root2 = leetcode::tree::create({"2", "2"});
    EXPECT_TRUE(s.leafSimilar(root1, root2));

    leetcode::tree::destroy(root1);
    leetcode::tree::destroy(root2);
  }

  {
    auto root1 = leetcode::tree::create({"1", "2", "3"});
    auto root2 = leetcode::tree::create({"1", "3", "2"});
    EXPECT_FALSE(s.leafSimilar(root1, root2));
    leetcode::tree::destroy(root1);
    leetcode::tree::destroy(root2);
  }

  {
    auto root1 = leetcode::tree::create(
        {"3", "5", "1", "6", "2", "9", "8", "null", "null", "7", "4"});
    auto root2 = leetcode::tree::create({"3", "5", "1", "6", "7", "4", "2",
                                         "null", "null", "null", "null", "null",
                                         "null", "9", "8"});
    EXPECT_TRUE(s.leafSimilar(root1, root2));
    leetcode::tree::destroy(root1);
    leetcode::tree::destroy(root2);
  }
}
