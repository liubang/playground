#include "highkyck/meta/scope_guard.h"

#include <iostream>

int main(int argc, char* argv[]) {
  {
    std::cout << "first scope begin" << std::endl;
    SCOPE_EXIT { std::cout << "first scope exit" << std::endl; };
    std::cout << "first scope end" << std::endl;
  }

  {
    std::cout << "second scope begin" << std::endl;
    SCOPE_EXIT { std::cout << "second scope exit" << std::endl; };
    goto END;
    std::cout << "second scope end" << std::endl;
  }

END:
  std::cout << "end" << std::endl;

  return 0;
}
