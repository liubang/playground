#include <stdio.h>

extern int prog();

int main(int argc, char *argv[]) {
  printf("prot() = %d\n", prog());
  return 0;
}
