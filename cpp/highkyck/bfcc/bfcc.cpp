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
#include "parser.h"
#include "print_visitor.h"

static constexpr char code[] = "  5 + 1- 3*4/2 ";

using Lexer = highkyck::bfcc::Lexer;
using Parser = highkyck::bfcc::Parser;
using PrintVisitor = highkyck::bfcc::PrintVisitor;

int main(int argc, char* argv[]) {
  // put your code here
  Lexer lexer(code);
  lexer.GetNextToken();
  Parser parser(&lexer);
  PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);

  return 0;
}
