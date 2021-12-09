#include <functional>
#include <iostream>

int foo(int a, int b, int c, int d) { return 1000 * a + 100 * b + 10 * c + d; }

// c++11 方式消元
void test_partial() {
  auto g = std::bind(foo, 3, 1, 4, std::placeholders::_1);
  std::cout << g(5) << '\n';
}

int main(int argc, char* argv[]) {
  test_partial();
  return 0;
}
