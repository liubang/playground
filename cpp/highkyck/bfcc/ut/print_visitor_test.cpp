#include "highkyck/bfcc/print_visitor.h"

#include <gtest/gtest.h>

#include "highkyck/bfcc/lexer.h"
#include "highkyck/bfcc/parser.h"

TEST(PrintVisitor, test_expr) {
  const char* code =
      " abc_123456 = 111; abc_123456; 125 + abc_123456 + (1 - 3 ) * 4 / 2;";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret,
            "abc_123456 = 111;abc_123456;125 + abc_123456 + 1 - 3"
            " * 4 / 2;\n");
}

TEST(PrintVisitor, test_relational_operator) {
  const char* code = "a==3;a!=3;a>3;a>=3;a<3;a<=3;";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, "a == 3;a != 3;a > 3;a >= 3;a < 3;a <= 3;\n");
}

TEST(PrintVisitor, test_if_else_single_stmt) {
  const char* code = "a=3; if (a!= 3) a=3; else a = a*a;";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, "a = 3;if (a != 3) a = 3; else a = a * a;\n");
}

TEST(PrintVisitor, test_if_else_multi_stmt) {
  const char* code = "a=3; if (a!= 3) {a=3; a = a+3;} else a = a*a;;a;";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, "a = 3;if (a != 3) {a = 3;a = a + 3;} else a = a * a;;a;\n");
}

TEST(PrintVisitor, test_while_stmt) {
  const char* code = "a=3; while (a <= 10) { a = a + 1; }";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, "a = 3;while (a <= 10) {a = a + 1;}\n");
}
