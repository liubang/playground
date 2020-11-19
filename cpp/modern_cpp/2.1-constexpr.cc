#include <iostream>

#define LEN 10

int len_foo() {
  int i = 2;
  return i;
}

constexpr int len_foo_constexpr() {
  return 5;
}

constexpr int fibonacci(const int n) {
  return n == 1 || n == 2 ? 1 : fibonacci(n - 1) + fibonacci(n - 2);
}

int main(int argc, char* argv[]) {
  char arr1[10];
  char arr2[LEN];

  int len = 10;
  // char arr3[len]; // 非法

  const int len2 = len + 1;
  // char arr4[len2]; // 非法
  constexpr int len2_constexpr = 1 + 2 + 3;
  char arr4[len2_constexpr];

  char arr5[len_foo_constexpr() + 1];

  return 0;
}
