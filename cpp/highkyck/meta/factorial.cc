#include <iostream>

namespace highkyck {
namespace meta {
template<int N>
struct Factorial
{
  static int const value = N * Factorial<N - 1>::value;
};

template<>
struct Factorial<1>
{
  static int const value = 1;
};
}  // namespace meta
}  // namespace highkyck

int main(int argc, char* argv[])
{
  std::cout << "Factorial<5>::value: " << highkyck::meta::Factorial<5>::value << '\n';
  std::cout << "Factorial<10>::value: " << highkyck::meta::Factorial<10>::value << '\n';
  return 0;
}
