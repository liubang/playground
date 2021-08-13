#include <iostream>

namespace highkyck {
struct S
{
  int c : 9;
  int d : 7;
};
}  // namespace highkyck

int main(int argc, char* argv[])
{
  // 4 Bytes = int
  // 32 bits
  std::cout << sizeof(highkyck::S) << "\n";
  return 0;
}
