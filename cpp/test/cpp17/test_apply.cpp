#include <iostream>
#include <string_view>
#include <tuple>

int sum(int a, int b, int c) {
  return a + b + c;
}

void print(std::string_view a, std::string_view b) {
  std::cout << "(" << a << "," << b << ")" << std::endl;
}

int main(int argc, char* argv[]) {
  std::tuple numbers{1, 2, 3};
  std::cout << std::apply(sum, numbers) << std::endl;

  std::tuple strs{"hello", "world"};
  std::apply(print, strs);

  return 0;
}
