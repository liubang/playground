#include <gtest/gtest.h>

#include <functional>
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

class BSTIterator {
 public:
  BSTIterator(TreeNode* node) { visit(node); }

  int next() {
    if (hasNext()) {
      return list_[idx_++];
    }
    return -1;
  }

  bool hasNext() { return idx_ < list_.size(); }

 private:
  void visit(TreeNode* node) {
    if (!node) {
      return;
    }
    visit(node->left);
    list_.push_back(node->val);
    visit(node->right);
  }

 private:
  std::vector<int> list_;
  int idx_{0};
};
}  // namespace

TEST(Leetcode, binary_search_tree_iterator) {
  TreeNode* root = new TreeNode(
      7, new TreeNode(3), new TreeNode(15, new TreeNode(9), new TreeNode(20)));
  BSTIterator it(root);

  EXPECT_EQ(3, it.next());
  EXPECT_EQ(7, it.next());
  EXPECT_TRUE(it.hasNext());
  EXPECT_EQ(9, it.next());
  EXPECT_TRUE(it.hasNext());
  EXPECT_EQ(15, it.next());
  EXPECT_TRUE(it.hasNext());
  EXPECT_EQ(20, it.next());
  EXPECT_FALSE(it.hasNext());

  // destroy tree
  using Destroctor = std::function<void(TreeNode*)>;
  Destroctor destroy = [&](TreeNode* node) {
    if (!node) {
      return;
    }
    destroy(node->left);
    destroy(node->right);
    delete node;
  };

  destroy(root);
}
