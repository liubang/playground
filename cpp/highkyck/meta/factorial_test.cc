#include <iostream>

#include "factorial.h"

int main(int argc, char* argv[]) {
  std::cout << "1! is : " << ::highkyck::detail::Factorial_v<1> << "\n";
  std::cout << "2! is : " << ::highkyck::detail::Factorial_v<2> << "\n";
  std::cout << "3! is : " << ::highkyck::detail::Factorial_v<3> << "\n";
  std::cout << "10! is : " << ::highkyck::detail::Factorial_v<10> << "\n";

  return 0;
}
