#include <iostream>

namespace {

enum T1 {
  T1_T0 = 0,
  T1_T1 = 1,
  T1_T2 = 2,
  T1_T3 = 3,
};

enum T2 {
  T2_T0 = 0,
  T2_T1 = 1,
  T2_T2 = 2,
  T2_T3 = 3,
};

struct Foo
{
  union {
    struct
    {
      T1 t1 : 4;
      T2 t2 : 4;
    } __attribute__((packed));
    char val_;
  } __attribute__((packed));

  explicit Foo(const char &val) : val_(val) {}

  Foo(T1 t1, T2 t2) : t1(t1), t2(t2) {}
};

static_assert(sizeof(Foo) == 1, "error");

}// namespace


int main(int argc, char *argv[])
{
  {
    char c = 1;
    c = (c << 4) | 1;
    auto f = Foo(c);

    std::cout << "t1: " << f.t1 << ", t2: " << f.t2 << "\n";
    std::cout << "val: " << static_cast<int>(f.val_) << "\n";
  }

  {
    auto f = Foo(T1::T1_T1, T2::T2_T3);
    std::cout << "t1: " << f.t1 << ", t2: " << f.t2 << "\n";
    std::cout << "val: " << static_cast<int>(f.val_) << "\n";
  }

  return 0;
}
