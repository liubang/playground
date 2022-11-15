//=====================================================================
//
// bfcc.cpp -
//
// Created by liubang on 2022/01/16 19:14
// Last Modified: 2022/01/16 19:14
//
//=====================================================================
#include <cstdio>
#include <iostream>

#include "lexer.h"

int main(int argc, char* argv[]) {
  if (argc != 2) {
    printf("usage: ./bfcc code\n");
    return 0;
  }

  FILE* fp = ::fopen(argv[1], "r");
  if (!fp) {
    std::cerr << "file open failed: " << argv[1] << "\n";
    return 0;
  }

  char buf[1024 * 10];
  size_t len = fread(buf, 1, sizeof(buf), fp);
  buf[len] = '\0';
  const char* source = buf;

  highkyck::bfcc::Lexer lexer(source);
  do {
    lexer.GetNextToken();
    std::cout << *(lexer.CurrentToken()) << std::endl;
  } while (lexer.CurrentToken()->type != highkyck::bfcc::TokenType::Eof);

  return 0;
}
