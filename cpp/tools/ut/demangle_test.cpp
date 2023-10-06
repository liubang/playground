//=====================================================================
//
// demangle_test.cpp -
//
// Created by liubang on 2023/10/06 23:16
// Last Modified: 2023/10/06 23:16
//
//=====================================================================
#include "cpp/meta/utils.h"
#include "cpp/tools/demangle.h"

int main(int argc, char* argv[]) {
    {
        int t;
        const int& s = t;
        pl::print(pl::demangle<decltype(s)>());
    }

    {
        int* const s = nullptr;
        pl::print(pl::demangle<decltype(s)>());
    }

    return 0;
}
