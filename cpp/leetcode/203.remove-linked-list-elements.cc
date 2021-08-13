#include <vector>
#include <gtest/gtest.h>
#include "includes/list.h"

namespace {
class Solution
{
public:
  using ListNode = leetcode::list::ListNode;
  ListNode* removeElements(ListNode* head, int val)
  {
    if (!head) return head;
    while (head && head->val == val) {
      ListNode* tmp = head;
      head = head->next;
      delete tmp;
    }
    ListNode* pre = nullptr;
    ListNode* cur = head;
    while (cur) {
      if (cur->val == val) {
        ListNode* tmp = cur;
        cur = cur->next;
        pre->next = cur;
        delete tmp;
      }
      else {
        pre = cur;
        cur = cur->next;
      }
    }
    return head;
  }
};
}  // namespace

TEST(Leetcode, remove_linked_list_elements)
{
  Solution s;
  {
    auto* head = leetcode::list::create({1, 2, 6, 3, 4, 5, 6});
    auto* res = s.removeElements(head, 6);
    auto* exp = leetcode::list::create({1, 2, 3, 4, 5});
    EXPECT_TRUE(leetcode::list::equals(exp, res));

    leetcode::list::destroy(res);
    leetcode::list::destroy(exp);
  }
}
