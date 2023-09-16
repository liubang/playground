#include <bitset>
#include <cassert>
#include <iostream>

#include "cpp/tools/bits.h"

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
            OtherType ot : 4;
            SomeType st : 4;
        } __attribute__((packed));
        char val_;
    } __attribute__((packed));

    explicit Outer(char c) : val_(c) {}

    Outer(SomeType st, OtherType ot) : ot(ot), st(st) {}

} __attribute__((packed));

int main(int argc, char *argv[]) {
    static_assert(sizeof(Outer) == 1);
    {
        Outer outer(ST_B, OT_C);
        assert(outer.st == ST_B);
        assert(outer.ot == OT_C);
        std::cout << std::bitset<8>(outer.val_) << std::endl;
        assert(sizeof(outer) == 1);
    }

    {
        Outer outer(static_cast<char>(OT_C));
        std::cout << std::bitset<8>(outer.val_) << std::endl;
        std::cout << outer.st << std::endl;
        std::cout << outer.ot << std::endl;
        assert(outer.st == ST_A);
        assert(outer.ot == OT_C);
        assert(sizeof(outer) == 1);
    }

    return 0;
}
