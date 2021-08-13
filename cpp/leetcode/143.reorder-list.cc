#include <vector>
#include <gtest/gtest.h>

#include "includes/list.h"

namespace {
class Solution
{
public:
  using ListNode = leetcode::list::ListNode;

  void reorderList(ListNode* head)
  {
    if (!head) { return; }
    std::vector<ListNode*> vecs;
    ListNode* cur = head;
    while (cur) {
      vecs.push_back(cur);
      cur = cur->next;
    }
    int i = 0, j = vecs.size() - 1, count = 0;
    while (i < j) {
      if ((count & 1) == 0) { vecs[i++]->next = vecs[j]; }
      else {
        vecs[j--]->next = vecs[i];
      }
      count++;
    }
    vecs[i]->next = nullptr;
  }
};
}  // namespace

TEST(Leetcode, reorder_list)
{
  using ListNode = leetcode::list::ListNode;
  Solution s;

  {
    std::vector<int> nodes = {1, 2, 3, 4};
    ListNode* head = leetcode::list::create(nodes);
    std::vector<int> exps = {1, 4, 2, 3};
    ListNode* exp = leetcode::list::create(exps);
    s.reorderList(head);
    EXPECT_TRUE(leetcode::list::equals(exp, head));

    leetcode::list::destroy(head);
    leetcode::list::destroy(exp);
  }

  {
    std::vector<int> nodes = {1, 2, 3, 4, 5};
    ListNode* head = leetcode::list::create(nodes);
    std::vector<int> exps = {1, 5, 2, 4, 3};
    ListNode* exp = leetcode::list::create(exps);
    s.reorderList(head);
    EXPECT_TRUE(leetcode::list::equals(exp, head));

    leetcode::list::destroy(head);
    leetcode::list::destroy(exp);
  }
}
