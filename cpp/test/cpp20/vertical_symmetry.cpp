#include <algorithm>
#include <array>
#include <iostream>
#include <ranges>

namespace test::cpp20 {

template <typename T, std::size_t width>
constexpr std::array<T, width * 2> mirror(std::array<T, width> a) {
  std::array<T, width* 2> out = {};
  std::ranges::copy(a, std::ranges::begin(out));
  std::ranges::copy(a, std::ranges::rbegin(out));
  return out;
}

}  // namespace test::cpp20

int main(int argc, char* argv[]) {
  // put your code hare
  auto a = std::array{1, 2, 3, 4};

  std::cout << "print a:" << std::endl;

  std::for_each(a.begin(), a.end(),
                [](const auto& e) { std::cout << e << std::endl; });
  auto b = test::cpp20::mirror(a);

  std::cout << "print b:" << std::endl;

  std::for_each(b.begin(), b.end(),
                [](const auto& e) { std::cout << e << std::endl; });
  return 0;
}
