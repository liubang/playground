#include <cstdio>

void test1() {
  int x = 42;
  auto f = [&]() { ::printf("x: %d\n", x); };
  f();
  ::printf("x: %d\n", f.__x);
}

int main(int argc, char* argv[]) {
  test1();
  return 0;
}
