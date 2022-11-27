#include <cstdlib>
namespace highkyck {
namespace detail {

template <std::size_t N>
constexpr std::size_t fibonacci = fibonacci<N - 1> + fibonacci<N - 2>;

template <>
constexpr std::size_t fibonacci<0> = 0;

template <>
constexpr std::size_t fibonacci<1> = 1;

template <std::size_t N>
constexpr double golden_ratio = fibonacci<N + 1> * 1.0 / fibonacci<N>;

}  // namespace detail
}  // namespace highkyck
