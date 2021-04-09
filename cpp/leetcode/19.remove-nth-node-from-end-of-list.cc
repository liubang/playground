#include <vector>
#include <gtest/gtest.h>
#include "includes/list.h"

namespace {
class Solution {
 public:
  using ListNode = leetcode::list::ListNode;
  ListNode* removeNthFromEnd(ListNode* head, int n) {
    std::vector<ListNode*> lists;
    ListNode* cur = head;
    while (cur) {
      lists.push_back(cur);
      cur = cur->next;
    }
    int size = lists.size();
    if (n == size) {
      ListNode* ret = head->next;
      delete head;
      return ret;
    }
    int idx = size - n - 1;
    ListNode* next = lists[idx]->next;
    lists[idx]->next = lists[idx]->next->next;
    delete next;
    return lists[0];
  }
};
} // namespace

TEST(Leetcode, remove_nth_node_from_end_of_list) {
  Solution s;

  {
    std::vector<int> nodes = {1, 2, 3, 4, 5};
    auto head = leetcode::list::create(nodes);
    auto ret = s.removeNthFromEnd(head, 1);
    EXPECT_EQ(1, ret->val);
    EXPECT_EQ(2, ret->next->val);
    EXPECT_EQ(3, ret->next->next->val);
    EXPECT_EQ(4, ret->next->next->next->val);
    leetcode::list::destroy(ret);
  }

  {
    std::vector<int> nodes = {1, 2, 3, 4, 5};
    auto head = leetcode::list::create(nodes);
    auto ret = s.removeNthFromEnd(head, 5);
    EXPECT_EQ(2, ret->val);
    EXPECT_EQ(3, ret->next->val);
    EXPECT_EQ(4, ret->next->next->val);
    EXPECT_EQ(5, ret->next->next->next->val);
    leetcode::list::destroy(ret);
  }
}
