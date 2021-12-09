#include <gtest/gtest.h>

#include <functional>

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
  bool isSubStructure(TreeNode* A, TreeNode* B) {
    if (!A || !B) {
      return false;
    }
    // 找到相等的节点，然后判断以此节点为根的子树是否相等
    // 若相等，则直接返回true
    // 否则找下一个相等的节点
    if (A->val == B->val) {
      return isEqual(A, B);
    }
    return isSubStructure(A->left, B) || isSubStructure(A->right, B);
  }

 private:
  bool isEqual(TreeNode* a, TreeNode* b) {
    // b已经遍历完了，不管a是否遍历完，a都包含了b
    if (!b) {
      return true;
    }
    if (a && b && a->val == b->val) {
      return isEqual(a->left, b->left) && isEqual(a->right, b->right);
    }
    return false;
  }
};
}  // namespace

TEST(Leetcode, shu_de_zi_jie_gou_lcof) {
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

  /*
   *     3
   *    / \
   *   4   5
   *  / \
   * 1   2
   *
   */
  TreeNode* root = new TreeNode(
      3, new TreeNode(4, new TreeNode(1), new TreeNode(2)), new TreeNode(5));

  TreeNode* sub = new TreeNode(3, new TreeNode(4), nullptr);
  TreeNode* notsub = new TreeNode(3, new TreeNode(1), nullptr);

  EXPECT_TRUE(s.isSubStructure(root, sub));
  EXPECT_FALSE(s.isSubStructure(root, notsub));

  destroy(root);
  destroy(sub);
  destroy(notsub);
}
