#include <iostream>

#include "folly/ScopeGuard.h"

int add(int a, int b) {
  SCOPE_EXIT {
    std::cout << "add exist" << std::endl;
  };
  return a + b;
}

int main(int argc, char* argv[]) {
  SCOPE_EXIT {
    std::cout << "main exist" << std::endl;
  };
  std::cout << add(1, 2) << std::endl;
  for (;;) {
  }
  return 0;
}
