//=====================================================================
//
// laxer.h -
//
// Created by liubang on 2023/09/17 23:05
// Last Modified: 2023/09/17 23:05
//
//=====================================================================
#pragma once

#include <string>

namespace pl::llvm {

// clang-format off
enum Token {
    tok_eof        = -1,
    tok_def        = -2,
    tok_extern     = -3,
    tok_identifier = -4,
    tok_number     = -5,
};
// clang-format on

int gettok();
double get_num_val();
int get_next_token();
int curtok();
const std::string& identifier_string();

} // namespace pl::llvm
