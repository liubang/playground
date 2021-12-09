#include <gtest/gtest.h>

#include <vector>

namespace {
struct TreeNode {
  int val;
  TreeNode* left;
  TreeNode* right;
  TreeNode(int x) : val(x), left(NULL), right(NULL) {}
};

struct ListNode {
  int val;
  ListNode* next;
  ListNode(int x) : val(x), next(NULL) {}
};

class Solution {
 public:
  std::vector<ListNode*> listOfDepth(TreeNode* tree) {
    std::vector<ListNode*> ret;
    visit(&ret, tree, 0);
    return ret;
  }

 private:
  void visit(std::vector<ListNode*>* ret, TreeNode* node, int dep) {
    if (node == nullptr) {
      return;
    }
    if (ret->size() >= (dep + 1)) {
      ListNode* cur = (*ret)[dep];
      while (cur->next != nullptr) {
        cur = cur->next;
      }
      cur->next = new ListNode(node->val);
    } else {
      ret->push_back(new ListNode(node->val));
    }
    visit(ret, node->left, dep + 1);
    visit(ret, node->right, dep + 1);
  }
};
}  // namespace

TEST(Leetcode, list_of_depth_lcci) {
  Solution s;
  TreeNode* tree = new TreeNode(1);
  tree->left = new TreeNode(2);
  tree->right = new TreeNode(3);
  tree->left->left = new TreeNode(4);
  tree->left->right = new TreeNode(5);
  tree->right->right = new TreeNode(7);
  tree->left->left->left = new TreeNode(8);
  auto ret = s.listOfDepth(tree);
  std::vector<std::vector<int>> exp = {{1}, {2, 3}, {4, 5, 7}, {8}};
  EXPECT_EQ(ret.size(), exp.size());
  for (int i = 0; i < ret.size(); ++i) {
    int c = 0;
    ListNode* cur = ret[i];
    while (cur != nullptr || c < exp[i].size()) {
      EXPECT_EQ(cur->val, exp[i][c]);
      cur = cur->next;
      c++;
    }
  }
  // destroy...
}
