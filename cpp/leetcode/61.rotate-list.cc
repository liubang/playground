#include <gtest/gtest.h>

#include <vector>

namespace {
struct ListNode {
  int val;
  ListNode* next;
  ListNode() : val(0), next(nullptr) {}
  ListNode(int x) : val(x), next(nullptr) {}
  ListNode(int x, ListNode* next) : val(x), next(next) {}
};

class Solution {
 public:
  ListNode* rotateRight(ListNode* head, int k) {
    if (!head || !head->next) {
      return head;
    }
    int len = 0;
    ListNode* cur = head;
    while (cur) {
      len++;
      cur = cur->next;
    }
    int move = k % len;
    if (move == 0) {
      return head;
    }
    int i = 0;
    cur = head;
    while (i < (len - move - 1)) {
      cur = cur->next;
      i++;
    }
    ListNode* h = cur->next;
    cur->next = nullptr;
    cur = h;
    while (cur->next) {
      cur = cur->next;
    }
    cur->next = head;
    return h;
  }
};
}  // namespace

TEST(Leetcode, rotate_list) {
  Solution s;
  ListNode* node = new ListNode(
      1, new ListNode(2, new ListNode(3, new ListNode(4, new ListNode(5)))));

  {
    ListNode* ret = s.rotateRight(node, 2);
    std::vector<int> exp = {4, 5, 1, 2, 3};
    ListNode* cur = ret;
    int i = 0;
    while (cur) {
      EXPECT_EQ(exp[i], cur->val);
      cur = cur->next;
      i++;
    }
  }

  ListNode* cur = node;
  while (cur) {
    ListNode* tmp = cur;
    cur = cur->next;
    delete tmp;
  }
}
