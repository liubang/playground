#include "cpp/tools/bits.h"

#include <iostream>

int main(int argc, char* argv[]) {
  uint64_t i = 26;
  uint64_t j = pl::tools::Endian::swap(i);
  std::cout << j << std::endl;
  return 0;
}
