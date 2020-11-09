#include "singleton.h"
#include <stdio.h>

extern "C" void g() {
  printf("%s: %p\n", __func__, &Singleton::Instance());
}
