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

int main(int argc, char* argv[]) {
  // put your code here
  const char* code = " 125 + 1 - 3 * 4 / 2";

  highkyck::bfcc::Lexer lexer(code);

  do {
    lexer.GetNextToken();
    std::cout << lexer.CurrentToken()->Content() << std::endl;
  } while (lexer.CurrentToken()->Kind() != highkyck::bfcc::TokenKind::Eof);

  return 0;
}
