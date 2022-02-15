//=====================================================================
//
// bfcc.cpp -
//
// Created by liubang on 2022/01/16 19:14
// Last Modified: 2022/01/16 19:14
//
//=====================================================================
#include <iostream>

#include "lexer.h"

int main(int argc, char* argv[])
{
    // put your code here
    const char*           source = argv[1];
    highkyck::bfcc::Lexer lexer(source);
    do {
        lexer.GetNextToken();
        std::cout << *(lexer.CurrentToken()) << std::endl;
    } while (lexer.CurrentToken()->Type() != highkyck::bfcc::TokenType::Eof);

    return 0;
}
