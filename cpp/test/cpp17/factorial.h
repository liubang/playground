#pragma once

namespace test {
namespace cpp17 {

template <int N>
struct Factorial {
  static constexpr int value = N * Factorial<N - 1>::value;
};

template <>
struct Factorial<1> {
  static constexpr int value = 1;
};

template <int N>
inline constexpr int Factorial_v = Factorial<N>::value;

template <int N>
inline constexpr int factorial() {
  if constexpr (N >= 2) {
    return N * factorial<N - 1>();
  } else {
    return N;
  }
}

}  // namespace cpp17
}  // namespace test
