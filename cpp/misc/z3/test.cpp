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

#include "z3.h"

#include <cassert>

int main(int argc, char* argv[]) {
    {
        pl::curve::Z3 z3(1, 2, 4);
        std::cout << z3;
        std::cout << std::bitset<64>(pl::curve::Z3::split((1ULL << 63) - 1)) << '\n';
        std::cout << std::bitset<64>(pl::curve::Z3::MAX_MASK) << '\n';
    }

    {
        pl::curve::Z3 z3(1, 0, 0);
        std::cout << z3;
        assert(z3.val() == 1);
        auto [x, y, z] = z3.decode();
        assert(x == 1);
        assert(y == 0);
        assert(z == 0);
    }

    {
        pl::curve::Z3 z3(0, 1, 0);
        std::cout << z3;
        assert(z3.val() == 2);
    }

    {
        pl::curve::Z3 z3(0, 0, 1);
        std::cout << z3;
        assert(z3.val() == 4);
    }

    {
        pl::curve::Z3 z3(1, 1, 1);
        std::cout << z3;
        assert(z3.val() == 7);
        auto [x, y, z] = z3.decode();
        assert(x == 1);
        assert(y == 1);
        assert(z == 1);
    }

    return 0;
}
