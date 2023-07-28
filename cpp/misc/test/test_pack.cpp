#include <cassert>
#include <iostream>

enum SomeType {
  ST_A = 0,
  ST_B = 1,
  ST_C = 2,
};

enum OtherType {
  OT_A = 0,
  OT_B = 1,
  OT_C = 2,
  OT_D = 3,
};

struct Outer {
  union {
    struct {
      SomeType st : 4;
      OtherType ot : 4;
    } __attribute__((packed));
    char val_;
  };

  explicit Outer(char c) : val_(c) {}

  Outer(SomeType st, OtherType ot) : st(st), ot(ot) {}
};

int main(int argc, char *argv[]) {
  static_assert(sizeof(Outer) == 1);
  {
    Outer outer(ST_B, OT_C);
    assert(outer.st == ST_B);
    assert(outer.ot == OT_C);
    assert(sizeof(outer) == 1);
  }

  {
    Outer outer(static_cast<char>(OT_C));
    assert(outer.st == ST_A);
    assert(outer.ot == OT_C);
    assert(sizeof(outer) == 1);
  }

  return 0;
}
