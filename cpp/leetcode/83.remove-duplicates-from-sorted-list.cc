#include <gtest/gtest.h>

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
  ListNode* deleteDuplicates(ListNode* head) {
    if (!head || !head->next) {
      return head;
    }
    ListNode* cur = head;
    ListNode* next = head->next;
    for (;;) {
      while (next && cur->val == next->val) {
        ListNode* tmp = cur->next;
        cur->next = next->next;
        next = next->next;
        delete tmp;
      }
      if (!next) {
        break;
      }
      cur = cur->next;
      next = next->next;
    }
    return head;
  }
};
} // namespace

TEST(Leetcode, remove_duplicates_form_sorted_list) {
  Solution s;
  {
    // 1, 1, 2
    ListNode* head = new ListNode(1, new ListNode(1, new ListNode(2)));
    ListNode* ret = s.deleteDuplicates(head);
    EXPECT_EQ(1, ret->val);
    EXPECT_EQ(2, ret->next->val);
    ListNode* cur = ret;
    while (cur) {
      ListNode* next = cur->next;
      delete cur;
      cur = next;
    }
  }
}
