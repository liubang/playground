// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#include <bitset>
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
            OtherType ot : 4;
            SomeType st  : 4;
        } __attribute__((packed));
        char val_;
    } __attribute__((packed));

    explicit Outer(char c) : val_(c) {}

    Outer(SomeType st, OtherType ot) : ot(ot), st(st) {}

} __attribute__((packed));

int main(int argc, char* argv[]) {
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
