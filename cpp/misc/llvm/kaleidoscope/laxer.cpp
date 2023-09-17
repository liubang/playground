//=====================================================================
//
// laxer.cpp -
//
// Created by liubang on 2023/09/17 23:16
// Last Modified: 2023/09/17 23:16
//
//=====================================================================
#include "cpp/misc/llvm/kaleidoscope/laxer.h"

#include <cctype>
#include <cstdlib>

namespace pl::llvm {

static std::string identifier_str;
static double num_val;
static int cur_tok;

const std::string &identifier_string() {
    return identifier_str;
}

int curtok() {
    return cur_tok;
}

double get_num_val() {
    return num_val;
}

int get_next_token() {
    return cur_tok = gettok();
}

int gettok() {
    static int last_char = ' ';
    while (std::isspace(last_char) != 0) {
        last_char = getchar();
    }
    // identifier: [a-zA-Z][a-zA-Z0-9]
    if (std::isalpha(last_char) != 0) {
        identifier_str = last_char;
        while (std::isalnum((last_char = getchar())) != 0) {
            identifier_str += last_char;
        }

        if (identifier_str == "def") {
            return tok_def;
        }

        if (identifier_str == "extern") {
            return tok_extern;
        }

        return tok_identifier;
    }
    // number: [0-9.]+
    if (std::isdigit(last_char) != 0 || last_char == '.') {
        std::string num_str;
        do {
            num_str += last_char;
            last_char = getchar();
        } while ((std::isdigit(last_char) != 0) || last_char == '.');

        num_val = std::strtod(num_str.c_str(), nullptr);
        return tok_number;
    }

    // comment
    if (last_char == '#') {
        do {
            last_char = getchar();
        } while (last_char != EOF && last_char != '\n' && last_char != '\r');

        if (last_char != EOF) {
            return gettok();
        }
    }

    if (last_char == EOF) {
        return tok_eof;
    }

    int this_char = last_char;
    last_char     = getchar();
    return this_char;
}

} // namespace pl::llvm
