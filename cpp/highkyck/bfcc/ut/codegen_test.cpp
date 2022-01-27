#include "highkyck/bfcc/codegen.h"

#include <gtest/gtest.h>

#include "highkyck/bfcc/lexer.h"
#include "highkyck/bfcc/parser.h"
#include "highkyck/bfcc/print_visitor.h"

TEST(PrintVisitor, VisitorProgram) {
  const char* code = " 125 + (1 - 3 ) * 4 / 2";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, " 2  4  3  1  -  *  /  125  + \n");
}

// TEST(CodeGen, VisitorProgram) {}
