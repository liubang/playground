#include <stdlib.h>

#define DIM(x) (sizeof(x) / sizeof((x)[0]))

static int cmp(const void *a, const void *b) {
  const int *inta = (int *)a;
  const int *intb = (int *)b;
  return *inta - *intb;
}
