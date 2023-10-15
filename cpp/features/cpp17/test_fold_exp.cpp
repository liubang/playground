//=====================================================================
//
// test_fold_exp.cpp -
//
// Created by liubang on 2023/10/15 15:19
// Last Modified: 2023/10/15 15:19
//
//=====================================================================

#include <cstdio>
#include <iostream>

template <typename... Ts> auto func(Ts... ts) {
    // print number of parameters
    std::cout << sizeof...(ts) << std::endl;
    // print each parameter
    (printf("%d\n", ts), ...);
    return (0 + ... + ts);
}

int main(int argc, char* argv[]) {
    std::cout << func() << "\n";
    std::cout << func(1, 2) << "\n";
    std::cout << func(1, 2, 3) << "\n";

    return 0;
}
