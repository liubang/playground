#include "utils.h"
#include "traits.h"

int main(int argc, char* argv[]) {
  using arr345 = std::array<std::array<std::array<int, 3>, 4>, 5>;

  // 这里的纬度是逆序的
  static_assert(
      highkyck::meta::is_same_v<arr345,
                                highkyck::meta::Array<int, 5, 4, 3>::type>);

  highkyck::meta::print(1, 2, 3, 4, 5);

  return 0;
}
