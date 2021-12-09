#include <gtest/gtest.h>

#include <iostream>
#include <vector>

namespace {
class NestedInteger {
 public:
  NestedInteger(int value) : value_(value), is_integer_(true) {}

  NestedInteger(const std::vector<NestedInteger>& list)
      : is_integer_(false), list_(list) {}

  NestedInteger(const std::vector<int>& ints) {
    for (int i : ints) {
      list_.emplace_back(i);
    }
    is_integer_ = false;
  }

  NestedInteger(const NestedInteger&) = default;
  NestedInteger& operator=(const NestedInteger&) = default;
  NestedInteger(NestedInteger&&) = default;

  bool isInteger() const { return is_integer_; }

  int getInteger() const { return value_; }

  const std::vector<NestedInteger>& getList() const { return list_; }

 private:
  int value_{0};
  bool is_integer_{true};
  std::vector<NestedInteger> list_;
};

class NestedIterator {
 public:
  NestedIterator(const std::vector<NestedInteger>& nestedList) {
    visit(nestedList);
  }

  int next() { return list_[idx_++]; }

  bool hasNext() { return idx_ < list_.size(); }

 private:
  void visit(const std::vector<NestedInteger>& nestedList) {
    for (auto& nestedInteger : nestedList) {
      if (nestedInteger.isInteger()) {
        list_.push_back(nestedInteger.getInteger());
      } else {
        visit(nestedInteger.getList());
      }
    }
  }

 private:
  std::vector<int> list_;
  int idx_{0};
};
}  // namespace

TEST(Leetcode, flatten_nested_list_iterator) {
  {
    // [[1,1],2,[1,1]]
    std::vector<NestedInteger> inputs;
    std::vector<int> output;
    std::vector<int> exp = {1, 1, 2, 1, 1};
    inputs.emplace_back(std::vector<int>{1, 1});
    inputs.emplace_back(2);
    inputs.emplace_back(std::vector<int>{1, 1});

    NestedIterator it(inputs);
    while (it.hasNext()) {
      output.push_back(it.next());
    }
    EXPECT_EQ(exp, output);
  }

  {
    // [1,[4,[6]]]
    std::vector<NestedInteger> inputs;
    std::vector<int> output;
    std::vector<int> exp = {1, 4, 6};
    inputs.emplace_back(1);

    std::vector<NestedInteger> inner;
    inner.emplace_back(4);
    inner.emplace_back(std::vector<int>{6});
    inputs.emplace_back(inner);

    NestedIterator it(inputs);
    while (it.hasNext()) {
      output.push_back(it.next());
    }
    EXPECT_EQ(exp, output);
  }
}
