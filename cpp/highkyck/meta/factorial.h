namespace highkyck {
namespace detail {

template <int N>
struct Factorial {
  constexpr static int value = N * Factorial<N - 1>::value;
};

template <>
struct Factorial<1> {
  constexpr static int value = 1;
};

template <int N>
constexpr int Factorial_v = Factorial<N>::value;

}  // namespace detail
}  // namespace highkyck
