#include "includes/list.h"
#include <gtest/gtest.h>
#include <vector>

namespace {
class Solution
{
public:
  using ListNode = leetcode::list::ListNode;
  ListNode* reverseBetween(ListNode* head, int left, int right)
  {
    int s = 1, e = 1;
    ListNode* pre = nullptr;
    ListNode* cur = head;
    while (s < left && e < right && cur) {
      pre = cur;
      cur = cur->next;
      s++, e++;
    }
    ListNode* ss = pre;
    ListNode* se = cur;
    pre = nullptr;
    while (e <= right && cur) {
      ListNode* next = cur->next;
      cur->next = pre;
      pre = cur;
      cur = next;
      e++;
    }
    if (ss) { ss->next = pre; }
    se->next = cur;
    return ss ? head : pre;
  }
};
}  // namespace

TEST(Leetcode, reverse_linked_list_ii)
{
  Solution s;
  {
    auto head = leetcode::list::create({1, 2, 3, 4, 5});
    auto ret = s.reverseBetween(head, 2, 4);
    auto exp = leetcode::list::create({1, 4, 3, 2, 5});
    EXPECT_TRUE(leetcode::list::equals(exp, ret));

    leetcode::list::destroy(ret);
    leetcode::list::destroy(exp);
  }
}
