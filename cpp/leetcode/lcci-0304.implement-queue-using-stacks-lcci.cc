#include <gtest/gtest.h>

#include <stack>

namespace {
class MyQueue {
 public:
  MyQueue() = default;
  virtual ~MyQueue() = default;

  void push(int x) { st1_.push(x); }

  int pop() {
    if (st2_.empty()) {
      while (!st1_.empty()) {
        st2_.push(st1_.top());
        st1_.pop();
      }
    }
    int ret = st2_.top();
    st2_.pop();
    return ret;
  }

  int peek() {
    if (st2_.empty()) {
      while (!st1_.empty()) {
        st2_.push(st1_.top());
        st1_.pop();
      }
    }
    return st2_.top();
  }

  bool empty() { return st1_.empty() && st2_.empty(); }

 private:
  std::stack<int> st1_;
  std::stack<int> st2_;
};
}  // namespace

TEST(Leetcode, implement_queue_using_stacks_lcci) {
  MyQueue my_queue;
  my_queue.push(1);
  my_queue.push(2);

  EXPECT_EQ(1, my_queue.peek());
  EXPECT_EQ(1, my_queue.pop());
  EXPECT_EQ(2, my_queue.peek());
  EXPECT_FALSE(my_queue.empty());
  EXPECT_EQ(2, my_queue.pop());
  EXPECT_TRUE(my_queue.empty());
}
