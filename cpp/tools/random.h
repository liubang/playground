//=====================================================================
//
// random.h -
//
// Created by liubang on 2023/05/31 00:04
// Last Modified: 2023/05/31 00:04
//
//=====================================================================
#pragma once

#include <algorithm>
#include <string>

namespace pl {

std::string random_string(size_t length) {
    auto randchar = []() -> char {
        const char charset[] = "0123456789"
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "abcdefghijklmnopqrstuvwxyz";
        const size_t max_index = (sizeof(charset) - 1);
        return charset[rand() % max_index];
    };
    std::string str(length, 0);
    std::generate_n(str.begin(), length, randchar);
    return str;
}

} // namespace pl
