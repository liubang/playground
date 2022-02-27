#include <stdio.h>

extern int test();

int main(int argc, char *argv[]) {
  printf("test() = %d\n", test());
  return 0;
}
