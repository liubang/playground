//=====================================================================
//
// parallel_invoke2.cpp -
//
// Created by liubang on 2023/06/18 00:11
// Last Modified: 2023/06/18 00:11
//
//=====================================================================
#include <tbb/parallel_invoke.h>

#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
    std::string s = "hello world";
    char ch = 'd';
    tbb::parallel_invoke(
        [&] {
            for (std::size_t i = 0; i < s.size() / 2; ++i) {
                if (s[i] == ch)
                    std::cout << "found!" << std::endl;
            }
        },
        [&] {
            for (std::size_t i = s.size() / 2; i < s.size(); ++i) {
                if (s[i] == ch)
                    std::cout << "found!" << std::endl;
            }
        });

    return 0;
}
