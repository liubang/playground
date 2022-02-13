#include <iostream>

#include "highkyck/bfcc/codegen.h"
#include "highkyck/bfcc/lexer.h"
#include "highkyck/bfcc/parser.h"

using Lexer = highkyck::bfcc::Lexer;
using Parser = highkyck::bfcc::Parser;
using CodeGen = highkyck::bfcc::CodeGen;

int main(int argc, char* argv[]) {
  if (argc != 2) {
    exit(1);
  }
  Lexer lexer(argv[1]);
  lexer.GetNextToken();
  Parser parser(&lexer);
  CodeGen codegen;
  auto root = parser.Parse();
  root->Accept(&codegen);
  std::cout << codegen.Code();
}
