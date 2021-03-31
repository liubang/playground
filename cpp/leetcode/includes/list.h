#pragma once

#include <vector>
#include <functional>

namespace leetcode {
namespace list {
struct ListNode {
  int val;
  ListNode* next;
  ListNode(int x, ListNode* next = nullptr) : val(x), next(next) {}
};

ListNode* create(const std::vector<int>& vecs) {
  if (vecs.empty()) {
    return nullptr;
  }
  ListNode* root = new ListNode(vecs[0]);
  ListNode* cur = root;
  for (int i = 1; i < vecs.size(); ++i) {
    cur->next = new ListNode(vecs[i]);
    cur = cur->next;
  }
  return root;
}

void destroy(ListNode* node) {
  if (!node) {
    return;
  }
  destroy(node->next);
  delete node;
}

} // namespace list
} // namespace leetcode
