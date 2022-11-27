#include <iostream>
#include <limits>

#include "fibonacci.h"

int main(int argc, char* argv[]) {
  std::cout << highkyck::detail::fibonacci<0> << '\n';
  std::cout << highkyck::detail::fibonacci<1> << '\n';
  std::cout << highkyck::detail::fibonacci<2> << '\n';
  std::cout << highkyck::detail::fibonacci<3> << '\n';
  std::cout << highkyck::detail::fibonacci<4> << '\n';
  std::cout << highkyck::detail::fibonacci<5> << '\n';
  std::cout << highkyck::detail::fibonacci<666> << '\n';
  std::cout << highkyck::detail::fibonacci<999> << '\n';

  std::cout.precision(std::numeric_limits<double>::max_digits10);
  std::cout << highkyck::detail::golden_ratio<6> << '\n';
  std::cout << highkyck::detail::golden_ratio<10> << '\n';
  std::cout << highkyck::detail::golden_ratio<20> << '\n';
  std::cout << highkyck::detail::golden_ratio<50> << '\n';

  return 0;
}
