#include "deducing_type.h"
#include <array>
#include <iostream>

int main(int argc, char* argv[])
{
  const int a[] = {1, 2, 3, 4, 5};
  std::array<int, highkyck::meta::array_size(a)> arr;
  std::cout << arr.size() << std::endl;

  return 0;
}
