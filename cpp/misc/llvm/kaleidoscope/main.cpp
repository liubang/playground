//=====================================================================
//
// main.cpp -
//
// Created by liubang on 2023/09/24 01:14
// Last Modified: 2023/09/24 01:14
//
//=====================================================================
#include "cpp/misc/llvm/kaleidoscope/ast.h"
#include "cpp/misc/llvm/kaleidoscope/laxer.h"

int main(int argc, char* argv[]) {
    fprintf(stderr, "ready> ");
    pl::llvm::get_next_token();
    pl::llvm::run();
    return 0;
}
