#include "includes/list.h"
#include <gtest/gtest.h>
#include <stack>

namespace {
class Solution
{
public:
    using ListNode = leetcode::list::ListNode;
    bool isPalindrome(ListNode* head)
    {
        if (!head || !head->next) {
            return true;
        }
        ListNode*             slow = head;
        ListNode*             fast = head;
        std::stack<ListNode*> stack;
        while (fast && fast->next) {
            stack.push(slow);
            slow = slow->next;
            fast = fast->next->next;
        }
        if (fast) {
            slow = slow->next;
        }
        while (slow) {
            if (stack.top()->val != slow->val) {
                return false;
            } else {
                stack.pop();
                slow = slow->next;
            }
        }
        return stack.empty();
    }
};
}  // namespace

TEST(Leetcode, palindrome_linked_list)
{
    using ListNode = leetcode::list::ListNode;
    Solution s;
    {
        ListNode* list = leetcode::list::create({1, 2, 2, 3, 2, 2, 1});
        EXPECT_TRUE(s.isPalindrome(list));
        leetcode::list::destroy(list);
    }

    {
        ListNode* list = leetcode::list::create({1, 2, 3, 4, 5});
        EXPECT_FALSE(s.isPalindrome(list));
        leetcode::list::destroy(list);
    }
}
