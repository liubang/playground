#include <gtest/gtest.h>

#include <stack>

namespace {
class SortedStack {
 public:
  SortedStack() = default;
  virtual ~SortedStack() = default;

  void push(int x) {
    if (st_.empty() || st_.top() >= x) {
      st_.push(x);
    } else {
      std::stack<int> temp;
      while (!st_.empty() && st_.top() < x) {
        temp.push(st_.top());
        st_.pop();
      }
      st_.push(x);
      while (!temp.empty()) {
        st_.push(temp.top());
        temp.pop();
      }
    }
  }

  void pop() {
    if (!st_.empty()) {
      st_.pop();
    }
  }

  int peek() { return st_.empty() ? -1 : st_.top(); }

  bool empty() { return st_.empty(); }

 private:
  std::stack<int> st_;
};
}  // namespace

TEST(Leetcode, sort_of_stack_lcci) {
  SortedStack stack;
  stack.push(1);
  stack.push(2);
  stack.push(3);

  EXPECT_EQ(1, stack.peek());
  stack.pop();
  EXPECT_EQ(2, stack.peek());
  stack.pop();
  EXPECT_EQ(3, stack.peek());
  EXPECT_FALSE(stack.empty());
  stack.pop();
  EXPECT_TRUE(stack.empty());
}
