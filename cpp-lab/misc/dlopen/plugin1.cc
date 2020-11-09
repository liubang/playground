#include "singleton.h"
#include <stdio.h>

extern "C" void f() {
  printf("%s: %p\n", __func__, &Singleton::Instance());
}
