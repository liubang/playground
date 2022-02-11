#include "yabf.h"

// x = std::getchar(); y = x + 3; std::putchar(y);
static constexpr auto add3 = highkyck::bf::parse(",>+++<[->+<]>.");

int main(int argc, char* argv[]) {
  unsigned char memory[1024] = {};
  highkyck::bf::execute<add3>(memory);
  return 0;
}
