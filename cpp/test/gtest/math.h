#pragma once

namespace test_gtest {
template <typename T>
class Math {
 public:
  static T add(T a, T b) { return a + b; }
};
}  // namespace test_gtest
