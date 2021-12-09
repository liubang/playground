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
  ListNode* deleteDuplicates(ListNode* head) {
    if (head == nullptr) {
      return head;
    }
    ListNode* dummy = new ListNode(0, head);
    ListNode* cur = dummy;
    while (cur->next != nullptr && cur->next->next != nullptr) {
      if (cur->next->val == cur->next->next->val) {
        int x = cur->next->val;
        while (cur->next != nullptr && cur->next->val == x) {
          cur->next = cur->next->next;
        }
      } else {
        cur = cur->next;
      }
    }
    return dummy->next;
  }
};
}  // namespace

TEST(Leetcode, remove_duplicates_from_sorted_list_ii) {
  Solution s;
  {
    // 1,2,3,3,4,4,5
    std::vector<int> input = {1, 2, 3, 3, 4, 4, 5};
    ListNode* head = new ListNode(input[0]);
    ListNode* c = head;
    for (int i = 1; i < input.size(); ++i) {
      c->next = new ListNode(input[i]);
      c = c->next;
    }
    std::vector<int> output = {1, 2, 5};
    ListNode* exp = new ListNode(1);
    ListNode* e = exp;
    for (int i = 1; i < output.size(); ++i) {
      e->next = new ListNode(output[i]);
      e = e->next;
    }
    ListNode* ret = s.deleteDuplicates(head);
    ListNode* cur = ret;
    ListNode* cure = exp;
    while (cur != nullptr || cure != nullptr) {
      EXPECT_EQ(cur->val, cure->val);
      cur = cur->next;
      cure = cure->next;
    }
  }

  {
    // 1,1,1,2,3
    std::vector<int> input = {1, 1, 1, 2, 3};
    ListNode* head = new ListNode(input[0]);
    ListNode* h = head;
    for (int i = 1; i < input.size(); ++i) {
      h->next = new ListNode(input[i]);
      h = h->next;
    }

    ListNode* exp = new ListNode(2, new ListNode(3));
    ListNode* ret = s.deleteDuplicates(head);

    ListNode* cur = ret;
    ListNode* cure = exp;
    while (cur != nullptr || cure != nullptr) {
      EXPECT_EQ(cur->val, cure->val);
      cur = cur->next;
      cure = cure->next;
    }
  }
}
