#include <stdio.h>

#include "singleton.h"

extern "C" void g() { printf("%s: %p\n", __func__, &Singleton::Instance()); }
