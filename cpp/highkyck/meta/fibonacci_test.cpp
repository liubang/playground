#include "fibonacci.h"

#include <iostream>

int main(int argc, char* argv[]) {
  std::cout << "fibonacci 1 is : "
            << ::highkyck::detail::Fibonacci_v<1> << "\n";
  std::cout << "fibonacci 2 is : "
            << ::highkyck::detail::Fibonacci_v<2> << "\n";
  std::cout << "fibonacci 3 is : "
            << ::highkyck::detail::Fibonacci_v<3> << "\n";
  std::cout << "fibonacci 4 is : "
            << ::highkyck::detail::Fibonacci_v<4> << "\n";
  std::cout << "fibonacci 5 is : "
            << ::highkyck::detail::Fibonacci_v<5> << "\n";
  std::cout << "fibonacci 6 is : "
            << ::highkyck::detail::Fibonacci_v<6> << "\n";
  std::cout << "fibonacci 100 is : "
            << ::highkyck::detail::Fibonacci_v<100> << "\n";
  std::cout << "fibonacci 888 is : "
            << ::highkyck::detail::Fibonacci_v<888> << "\n";
  return 0;
}
