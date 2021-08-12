#include <gtest/gtest.h>
#include "includes/list.h"

namespace {

class Solution
{
public:
  using ListNode = leetcode::list::ListNode;
  ListNode* deleteDuplicates(ListNode* head)
  {
    if (!head || !head->next) { return head; }
    ListNode* cur = head;
    while (cur) {
      while (cur->next && cur->val == cur->next->val) {
        ListNode* n = cur->next;
        cur->next = cur->next->next;
        delete n;
      }
      cur = cur->next;
    }
    return head;
  }
};
}   // namespace

TEST(Leetcode, remove_duplicates_form_sorted_list)
{
  using ListNode = leetcode::list::ListNode;
  Solution s;
  {
    // 1, 1, 2
    ListNode* head = new ListNode(1, new ListNode(1, new ListNode(2)));
    ListNode* ret = s.deleteDuplicates(head);
    EXPECT_EQ(1, ret->val);
    EXPECT_EQ(2, ret->next->val);
    leetcode::list::destroy(ret);
  }

  {
    ListNode* head = new ListNode(1);
    auto ret = s.deleteDuplicates(head);
    EXPECT_EQ(1, ret->val);
    leetcode::list::destroy(ret);
  }
}
