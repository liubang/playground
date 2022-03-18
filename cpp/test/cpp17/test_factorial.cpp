#include <iostream>

#include "factorial.h"

int main(int argc, char* argv[]) {
  std::cout << test::cpp17::Factorial_v<1> << std::endl;
  std::cout << test::cpp17::Factorial_v<2> << std::endl;
  std::cout << test::cpp17::Factorial_v<3> << std::endl;
  std::cout << test::cpp17::Factorial_v<4> << std::endl;

  std::cout << test::cpp17::factorial<1>() << std::endl;
  std::cout << test::cpp17::factorial<2>() << std::endl;
  std::cout << test::cpp17::factorial<3>() << std::endl;
  std::cout << test::cpp17::factorial<4>() << std::endl;

  return 0;
}
