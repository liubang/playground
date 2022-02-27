#include <cstdio>
#include <iostream>

namespace test::cpp17 {

template <typename H, typename... T>
constexpr auto add(H head, T... ts) {
  return (head + ... + ts);
}

template <typename... T>
void call_func_with_each_elem(T... ts) {
  (::printf("%d", ts), ...);
}

template <typename... T>
void call_func_with_each_elem_reverse_order(T... ts) {
  [[maybe_unused]] int dummy;
  (dummy = ... = (::printf("%d", ts), 0));
}

template <typename... T>
constexpr auto get_minimal_elem(T... ts) {
  auto min = (ts, ...);
  ((ts < min ? min = ts : min), ...);
  return min;
}

}  // namespace test::cpp17

void test_add() {
  constexpr int a = test::cpp17::add(1, 2, 3, 4, 5, 6);
  static_assert(a == 21);
}

void test_call_func_with_each_elem() {
  test::cpp17::call_func_with_each_elem(1, 2, 3, 4);
  std::cout << std::endl;
}

void test_call_func_with_each_elem_reverse_order() {
  test::cpp17::call_func_with_each_elem_reverse_order(1, 2, 3, 4);
  std::cout << std::endl;
}

void test_get_minimal_elem() {
  constexpr auto a = test::cpp17::get_minimal_elem(0, 1, 2, 3, 4, 5);
  static_assert(a == 0);
}

int main(int argc, char* argv[]) {
  test_add();
  test_call_func_with_each_elem();
  test_call_func_with_each_elem_reverse_order();
  test_get_minimal_elem();
  return 0;
}
