#include <complex>

// case 42: universal(forwarding) reference qualifier
template<typename T> decltype(auto) fwd0(T &&p){};// template forwarding reference
decltype(auto) fwd1 = [](auto &&p) {};// lambda forwarding reference

int main(int /*argc*/, char * /*argv*/[])
{
  using CT = std::complex<double>;

  // case 1: no qualifier
  CT p1{};

  // case 2: const qualifier
  const CT p2{ p1 };

  // case 3: volatile qualifier
  volatile CT p3{ p2 };

  // case 40: reference qualifier
  CT &p40 = p1;// lvalue reference
  CT &&p41 = CT{};// rvalue reference

  // case 42: universal reference qualifier
  auto &&p420 = p1;// auto&& bind to lvalue
  auto &&p421 = CT{};// auto&& bind to rvalue, lifetime extension

  // case 5: pointer qualifier
  CT *p50 = nullptr;
  // '*' left side 'const' means can not modify 'p1' through 'p51'
  // '*' right side 'cosnt' means can not modify 'p51' to point to other object than 'p1'
  const CT *const p51 = &p1;
  // p51 and p52 are equivalent
  CT const *const p52 = &p1;

  // case 6: array qualifier
  CT p6[2] = { p1, p1 };

  return 0;
}
