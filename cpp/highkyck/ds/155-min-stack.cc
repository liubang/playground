#include "common.h"
#include <stack>
#include <climits>

namespace ds {
class MinStack {
 public:
  MinStack() = default;
  ~MinStack() = default;

  void push(int x) {
    min_ = std::min(x, min_);
    stack_.push(std::make_pair(min_, x));
  }

  void pop() {
    stack_.pop();
    min_ = stack_.empty() ? INT_MAX : getMin();
  }

  int top() {
    return stack_.top().second;
  }

  int getMin() {
    return stack_.top().first;
  }

 private:
  int min_{INT_MAX};
  std::stack<std::pair<int, int>> stack_;
};
} // namespace ds

// ["MinStack","push","push","push","top","pop","getMin","pop","getMin","pop","push","top","getMin","push","top","getMin","pop","getMin"]
// [[],[2147483646],[2147483646],[2147483647],[],[],[],[],[],[],[2147483647],[],[],[-2147483648],[],[],[],[]]
// [null,null,null,null,2147483647,null,2147483646,null,2147483646,null,null,2147483647,2147483647,null,-2147483648,-2147483648,null,2147483647]
TEST(MinStack, test) {
  ds::MinStack min_stack;
  min_stack.push(2147483646);
  min_stack.push(2147483646);
  min_stack.push(2147483647);
  EXPECT_EQ(2147483647, min_stack.top());
  min_stack.pop();
  EXPECT_EQ(2147483646, min_stack.getMin());
  min_stack.pop();
  EXPECT_EQ(2147483646, min_stack.getMin());
  min_stack.pop();
  min_stack.push(2147483647);
  EXPECT_EQ(2147483647, min_stack.top());
  EXPECT_EQ(2147483647, min_stack.getMin());
  min_stack.push(-2147483648);
  EXPECT_EQ(-2147483648, min_stack.top());
  EXPECT_EQ(-2147483648, min_stack.getMin());
  min_stack.pop();
  EXPECT_EQ(2147483647, min_stack.getMin());
}
