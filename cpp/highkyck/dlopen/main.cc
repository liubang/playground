#include <cstdio>
#include <dlfcn.h>

int main(int argc, char* argv[])
{
  typedef int F();
  void* fd1 =
      dlopen("/home/liubang/workspace/laboratory/cpp/bazel-bin/misc/libplugin1.so", RTLD_LAZY);

  F* f = (F*)dlsym(fd1, "f");
  f();

  void* fd2 =
      dlopen("/home/liubang/workspace/laboratory/cpp/bazel-bin/misc/libplugin2.so", RTLD_LAZY);
  F* g = (F*)dlsym(fd2, "g");
  g();

  return 0;
}
