#include <unordered_set>
#include <gtest/gtest.h>
#include "includes/list.h"

namespace {
class Solution
{
public:
  using ListNode = leetcode::list::ListNode;

  ListNode* detectCycle(ListNode* head)
  {
    if (!head || !head->next) { return nullptr; }
    std::unordered_set<ListNode*> set;
    ListNode* cur = head;
    while (cur) {
      if (set.count(cur) > 0) { return cur; }
      set.insert(cur);
      cur = cur->next;
    }
    return nullptr;
  }
};
}  // namespace

TEST(Leetcode, linked_list_cycle_ii)
{
  using ListNode = leetcode::list::ListNode;
  Solution s;
  {
    ListNode* head = new ListNode(0);
    head->next = new ListNode(1, new ListNode(2, new ListNode(3, head)));
    EXPECT_EQ(head, s.detectCycle(head));

    delete head->next->next->next;
    delete head->next->next;
    delete head->next;
    delete head;
  }

  {
    ListNode* head = new ListNode(1, new ListNode(2, new ListNode(3, new ListNode(4))));
    EXPECT_EQ(nullptr, s.detectCycle(head));
    leetcode::list::destroy(head);
  }
}
