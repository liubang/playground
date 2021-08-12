#include <iostream>

namespace highkyck {
class A
{
public:
  virtual void func() { std::cout << "A func" << '\n'; }
  virtual ~A() {}
};

class B : public A
{
public:
  void func() { std::cout << "B func" << '\n'; }
};

struct Base
{
  virtual ~Base() = default;
};
struct Derived : Base
{};

struct Base1
{};
struct Derived1 : Base1
{};
}   // namespace highkyck

using namespace highkyck;

void test1()
{
  A a;
  A* bptr = new B();
  bptr->func();

  int* pa = (int*)&a;
  int* pb = (int*)bptr;

  *pb = *pa;
  bptr->func();
}

void test2()
{
  Base b1;
  Derived d1;

  const Base* pb = &b1;
  std::cout << typeid(*pb).name() << '\n';
  pb = &d1;
  std::cout << typeid(*pb).name() << '\n';
}

void test3()
{
  Base1 b1;
  Derived1 d1;
  const Base1* pb = &b1;
  std::cout << typeid(*pb).name() << '\n';
  pb = &d1;
  std::cout << typeid(*pb).name() << '\n';
}

int main(int argc, char* argv[])
{
  test1();
  test2();
  test3();
  return 0;
}
