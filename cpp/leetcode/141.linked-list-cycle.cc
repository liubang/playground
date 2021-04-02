#include <gtest/gtest.h>
#include "includes/list.h"

namespace {
class Solution {
 public:
  bool hasCycle(leetcode::list::ListNode* head) {
    if (!head || !head->next) {
      return false;
    }
    leetcode::list::ListNode* slow = head->next;
    leetcode::list::ListNode* fast = head->next->next;
    while (fast && slow) {
      if (fast == slow) {
        return true;
      }
      if (!fast->next) {
        return false;
      }
      slow = slow->next;
      fast = fast->next->next;
    }
    return false;
  }
};
} // namespace

TEST(Leetcode, linked_list_cycle) {
  Solution s;
  using ListNode = leetcode::list::ListNode;

  {
    ListNode* head = new ListNode(3);
    head->next = new ListNode(2, new ListNode(0, new ListNode(-4, head)));
    EXPECT_TRUE(s.hasCycle(head));
    delete head->next->next->next;
    delete head->next->next;
    delete head->next;
    delete head;
  }

  {
    ListNode* head =
        new ListNode(3, new ListNode(2, new ListNode(1, new ListNode(0))));
    EXPECT_FALSE(s.hasCycle(head));
    leetcode::list::destroy(head);
  }
}
