#pragma once

#include <vector>
#include <string>
#include <iostream>

namespace leetcode {
namespace tree {
struct TreeNode {
  int val;
  TreeNode* left;
  TreeNode* right;
  TreeNode(int x, TreeNode* left = nullptr, TreeNode* right = nullptr)
      : val(x), left(left), right(right) {}
};

void destroy(TreeNode* node) {
  if (!node) {
    return;
  }
  destroy(node->left);
  destroy(node->right);
  delete node;
}

// 根据层次遍历构造二叉树
TreeNode* create(const std::vector<std::string>& nodes) {
  int len = nodes.size(), current = 0;
  std::vector<TreeNode*> pNodes;
  TreeNode* pCurNode;
  for (int current = 0; current < nodes.size(); ++current) {
    if (nodes[current] == "null" || nodes[current] == "nullptr") {
      continue;
    } else {
      pCurNode = new TreeNode(std::stoi(nodes[current]));
    }
    if (current > 0) {
      int parentIdx = (current - 1) >> 1;
      if (current % 2 != 0) {
        // left
        pNodes[parentIdx]->left = pCurNode;
      } else {
        // right
        pNodes[parentIdx]->right = pCurNode;
      }
    }
    pNodes.push_back(pCurNode);
  }
  return pNodes[0];
}
} // namespace tree
} // namespace leetcode
