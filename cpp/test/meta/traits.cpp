#include "traits.h"
#include <string>

int main(int argc, char* argv[]) {
  static_assert(highkyck::meta::is_floating_point<float>::value);
  static_assert(highkyck::meta::is_floating_point<double>::value);
  static_assert(highkyck::meta::is_floating_point<long double>::value);
  static_assert(false == highkyck::meta::is_floating_point<int>::value);

  static_assert(highkyck::meta::is_same<int, int>::value);
  static_assert(highkyck::meta::is_same<std::string, std::string>::value);
  static_assert(false == highkyck::meta::is_same<int, long>::value);

  static_assert(highkyck::meta::is_same<
                int, highkyck::meta::remove_const_t<const int>>::value);

  static_assert(
      highkyck::meta::is_same<int, highkyck::meta::conditional<
                                       true, int, std::string>::type>::value);
  static_assert(
      highkyck::meta::is_same<
          std::string,
          highkyck::meta::conditional<false, int, std::string>::type>::value);

  static_assert(highkyck::meta::is_same_v<
                int, highkyck::meta::array_size<int[5]>::value_type>);
  static_assert(highkyck::meta::array_size<int[5]>::value == 5);

  return 0;
}
