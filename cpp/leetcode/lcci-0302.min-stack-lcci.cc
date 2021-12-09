#include <gtest/gtest.h>

#include <climits>
#include <stack>

namespace {
class MinStack {
 public:
  MinStack() = default;
  virtual ~MinStack() = default;

  void push(int x) { st_.emplace(x, std::min(x, getMin())); }

  void pop() { st_.pop(); }

  int top() {
    if (!st_.empty()) {
      return st_.top().first;
    }
    return INT_MIN;
  }

  int getMin() {
    if (!st_.empty()) {
      return st_.top().second;
    }
    return INT_MAX;
  }

 private:
  std::stack<std::pair<int, int>> st_;
};
}  // namespace

TEST(Leetcode, min_stack_lcci) {
  MinStack min_stack;
  min_stack.push(-2);
  min_stack.push(0);
  min_stack.push(-3);

  EXPECT_EQ(-3, min_stack.getMin());
  EXPECT_EQ(-3, min_stack.top());

  min_stack.pop();
  EXPECT_EQ(-2, min_stack.getMin());
  EXPECT_EQ(0, min_stack.top());
}
