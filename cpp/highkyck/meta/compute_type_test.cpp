#include "compute_type.h"

#include <iostream>

int main(int argc, char* argv[]) {
  int a = 10;
  ::highkyck::detail::PointerType<int>::type iptr = &a;
  ::highkyck::detail::PointerType_t<int> iptr1 = &a;
  return 0;
}
