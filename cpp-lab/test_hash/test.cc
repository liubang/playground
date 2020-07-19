#include <city.h>
#include <iostream>

int main(int argc, char* argv[]) {
  // uint64 CityHash64WithSeed(const char *buf, size_t len, uint64 seed);
  std::string s("hello");
  std::cout << CityHash64WithSeed(s.data(), s.size(), 0) << std::endl;
  return 0;
}
