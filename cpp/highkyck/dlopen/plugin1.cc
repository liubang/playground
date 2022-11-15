#include <stdio.h>

#include "singleton.h"

extern "C" void f() {
  printf("%s: %p\n", __func__, &Singleton::Instance());
}
