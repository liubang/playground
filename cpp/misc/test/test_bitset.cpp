#include <bitset>
#include <iostream>

int main(int argc, char *argv[]) {
  for (size_t i = 0; i < 64; ++i) {
    uint64_t a = static_cast<uint64_t>(1) << i;
    std::bitset<64> bs = a;
    std::cout << "i ==> " << i << ", a ==> " << bs << std::endl;
  }

  // 逐位字节反转
  uint64_t a = 10245567210864513;
  for (size_t i = 0; i < 64; ++i) {
    uint64_t b = static_cast<uint64_t>(1) << i;
    uint64_t c = a ^ b;
    std::bitset<64> bs = c;
    std::cout << bs << "\n";
  }

  return 0;
}
