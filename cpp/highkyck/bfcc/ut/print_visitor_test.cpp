// #include "highkyck/bfcc/codegen.h"

#include <gtest/gtest.h>

#include "highkyck/bfcc/lexer.h"
#include "highkyck/bfcc/parser.h"
#include "highkyck/bfcc/print_visitor.h"

TEST(PrintVisitor, VisitorProgram) {
  const char* code = " abc_123456 = 111; abc_123456; 125 + abc_123456 + (1 - 3 ) * 4 / 2;";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, " abc_123456  =  111 ; abc_123456 ; 125  +  abc_123456  +  1  -  3  *  4  /  2 ;\n");
}
