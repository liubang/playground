#include <cstring>
#include <iostream>

#include "murmurhash2.h"

int main(int argc, char* argv[])
{
  const char* data = "hello world";
  uint64_t seed = 0;
  highkyck::hash::CMurmurHash64 hasher;
  for (uint32_t i = 0; i < 4; ++i) {
    hasher.begin(seed);
    hasher.add(data, strlen(data), false);
    seed = hasher.end();
  }

  std::cout << seed << '\n';

  return 0;
}
