#pragma once
#include <cstdio>

class Singleton {
 public:
  static Singleton& Instance() {
    static Singleton s;
    return s;
  }

 private:
  Singleton() {
    printf("Singleton ctor\n");
  }
};
