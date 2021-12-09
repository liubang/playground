#include <gtest/gtest.h>

#include <vector>

namespace {

struct TreeNode {
  int val;
  TreeNode* left;
  TreeNode* right;
  TreeNode() : val(0), left(nullptr), right(nullptr) {}
  TreeNode(int x) : val(x), left(nullptr), right(nullptr) {}
  TreeNode(int x, TreeNode* left, TreeNode* right)
      : val(x), left(left), right(right) {}
};

class Solution {
 public:
  std::vector<std::vector<int>> pathSum(TreeNode* root, int targetSum) {
    ret_.clear();
    dfs(root, targetSum);
    return ret_;
  }

 private:
  void dfs(TreeNode* node, int target) {
    if (node) {
      target -= node->val;
      path_.push_back(node->val);
      if (target == 0 && !node->left && !node->right) {
        ret_.push_back(path_);
      } else {
        dfs(node->left, target);
        dfs(node->right, target);
      }
      // 回溯
      path_.pop_back();
    }
  }

 private:
  std::vector<int> path_;
  std::vector<std::vector<int>> ret_;
};
}  // namespace

TEST(Leetcode, path_sum_ii) {
  Solution s;
  {
    TreeNode* root = new TreeNode(
        5,
        new TreeNode(4, new TreeNode(11, new TreeNode(7), new TreeNode(2)),
                     nullptr),
        new TreeNode(8, new TreeNode(13),
                     new TreeNode(4, new TreeNode(5), new TreeNode(1))));
    std::vector<std::vector<int>> exp = {{5, 4, 11, 2}, {5, 8, 4, 5}};
    auto ret = s.pathSum(root, 22);
    EXPECT_EQ(exp, ret);
  }

  {
    TreeNode* root = new TreeNode(1, new TreeNode(2), new TreeNode(3));
    auto ret = s.pathSum(root, 5);
    for (auto r : ret) {
      for (auto i : r) {
        std::cout << i << "\n";
      }
    }
    EXPECT_TRUE(ret.empty());
  }
}
