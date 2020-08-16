#include <cstdio>
#include <climits>

// 分支消除

int vtod(int v) {
  // if (v < 10)
  //   return v + 48;
  // else
  //   return v + 55;
  // return v + (v < 10 ? 48 : 55);
  return v + 55 - (((v - 10) >> 15) & 7);
}

int xabs(int n) {
  // if (n >= 0)
  //   return n;
  // else
  //   return -n;
  // -x = ~x + 1;
  // ~x = x ^ -1
  int s = n >> (WORD_BIT - 1);
  return (n ^ s) - s;
}

int main(int argc, char* argv[]) {
  for (int i = 0; i < 16; ++i)
    ::std::printf("%c ", vtod(i));
  ::std::putchar('\n');

  ::std::printf("%d\n", xabs(+123));
  ::std::printf("%d\n", xabs(0));
  ::std::printf("%d\n", xabs(-123));
  return 0;
}
