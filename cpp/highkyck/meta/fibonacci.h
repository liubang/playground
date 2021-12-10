#include <cstdlib>
namespace highkyck {
namespace detail {

template <std::size_t N>
struct Fibonacci {
  constexpr static std::size_t value =
      Fibonacci<N - 1>::value + Fibonacci<N - 2>::value;
};

template <>
struct Fibonacci<0> {
  constexpr static std::size_t value = 0;
};

template <>
struct Fibonacci<1> {
  constexpr static std::size_t value = 1;
};

template <std::size_t N>
constexpr std::size_t Fibonacci_v = Fibonacci<N>::value;

}  // namespace detail
}  // namespace highkyck
