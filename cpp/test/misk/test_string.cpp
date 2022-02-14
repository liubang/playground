#include <iostream>
#include <string>
#include <string_view>

namespace {
inline constexpr std::string_view deepSeaPath{R"(something)"};
}

int main(int argc, char* argv[]) {
  std::cout << deepSeaPath << std::endl;

  std::string s = "helloworld";
  auto* d1 = s.c_str();
  auto* dd1 = d1;
  std::size_t l = 0;
  while (*dd1 != '\0') {
    std::cout << *dd1;
    dd1++;
    l++;
  }
  std::cout << ", " << l << std::endl;

  auto* d2 = s.data();
  auto* dd2 = d2;
  l = 0;
  while (*dd2 != '\0') {
    std::cout << *dd2;
    dd2++;
    l++;
  }
  std::cout << ", " << l << std::endl;

  return 0;
}
