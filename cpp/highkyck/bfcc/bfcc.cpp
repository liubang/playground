//=====================================================================
//
// bfcc.cpp -
//
// Created by liubang on 2022/01/16 19:14
// Last Modified: 2022/01/16 19:14
//
//=====================================================================
#include <iostream>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "print_visitor.h"

static constexpr char code[] = "  5 + (1- 3)*4/2 ";

using Lexer = highkyck::bfcc::Lexer;
using Parser = highkyck::bfcc::Parser;
using PrintVisitor = highkyck::bfcc::PrintVisitor;
using CodeGen = highkyck::bfcc::CodeGen;

void test_lexer() {
  Lexer lexer(code);
  lexer.GetNextToken();
  Parser parser(&lexer);
  PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
}

void test_codegen() {
  Lexer lexer(code);
  lexer.GetNextToken();
  Parser parser(&lexer);
  CodeGen codegen;
  auto root = parser.Parse();
  root->Accept(&codegen);
}

int main(int argc, char* argv[]) {
  // put your code here
  test_codegen();
  return 0;
}
