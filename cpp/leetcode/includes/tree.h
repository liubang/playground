#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace leetcode {
namespace tree {
struct TreeNode {
  int val;
  TreeNode *left;
  TreeNode *right;
  TreeNode(int x, TreeNode *left = nullptr, TreeNode *right = nullptr)
      : val(x), left(left), right(right) {}
};

void destroy(TreeNode *node) {
  if (!node) {
    return;
  }
  destroy(node->left);
  destroy(node->right);
  delete node;
}

// 根据层次遍历构造二叉树
TreeNode *create(const std::vector<std::string> &nodes) {
  std::vector<TreeNode *> pNodes;
  TreeNode *pCurNode;
  for (int current = 0; current < nodes.size(); ++current) {
    if (nodes[current] == "null" || nodes[current] == "nullptr" ||
        nodes[current] == "NULL" || nodes[current] == "nil" ||
        nodes[current] == "Nil") {
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

bool equals(TreeNode *lhs, TreeNode *rhs) {
  if (lhs && rhs) {
    return lhs->val == rhs->val && equals(lhs->left, rhs->left) &&
           equals(lhs->right, rhs->right);
  }
  return lhs == rhs;
}

enum class Order {
  PRE_ORDER,
  IN_ORDER,
  POST_ORDER,
};

void print(TreeNode *node, Order order = Order::PRE_ORDER) {
  std::vector<int> seqs;
  std::function<void(TreeNode *, Order)> visit = [&](TreeNode *node,
                                                     Order order) {
    if (!node) {
      return;
    }
    if (order == Order::PRE_ORDER) {
      seqs.push_back(node->val);
      visit(node->left, order);
      visit(node->right, order);
    } else if (order == Order::IN_ORDER) {
      visit(node->left, order);
      seqs.push_back(node->val);
      visit(node->right, order);
    } else {
      visit(node->left, order);
      visit(node->right, order);
      seqs.push_back(node->val);
    }
  };

  visit(node, order);
  std::cout << '[';
  int count = 0;
  std::for_each(seqs.begin(), seqs.end(), [&](int v) {
    std::cout << v;
    if (count++ < seqs.size() - 1) {
      std::cout << ',';
    }
  });
  std::cout << ']' << std::endl;
}

} // namespace tree
} // namespace leetcode
