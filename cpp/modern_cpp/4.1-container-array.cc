#include <algorithm>
#include <array>
#include <iostream>

int main(int argc, char* argv[]) {
  std::array<int, 4> arr = {1, 5, 3, 4};
  std::cout << "是否为空：" << std::boolalpha << arr.empty() << std::endl;
  std::cout << "size: " << arr.size() << std::endl;

  // foreach
  std::cout << "排序前：" << std::endl;
  for (auto& el : arr) {
    std::cout << el << std::endl;
  }

  // lambda 排序
  std::sort(arr.begin(), arr.end(), [](int a, int b) { return b < a; });

  // foreach
  std::cout << "排序后：" << std::endl;
  for (auto& el : arr) {
    std::cout << el << std::endl;
  }

  return 0;
}
