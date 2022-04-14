#include <iostream>

void func1() {}

void func() { func1(); }

int main(int argc, char *argv[])
{
  func();
  return 0;
}
