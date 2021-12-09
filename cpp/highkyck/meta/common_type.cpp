#include <iostream>
#include <string>
#include <typeinfo>
#include <utility>

#include "common.h"

int main(int argc, char* argv[]) {
  uint32_t a;
  int32_t b;
  std::common_type<decltype(a), decltype(b)>::type c;
  std::common_type_t<int, uint32_t> d;

  std::cout << "the type of a is " << typeid(a).name() << std::endl;
  std::cout << "the type of b is " << typeid(b).name() << std::endl;
  std::cout << "the type of c is " << typeid(c).name() << std::endl;

  std::cout << "the type of a is "
            << ::highkyck::detail::type_name<decltype(a)>() << std::endl;

  return 0;
}
