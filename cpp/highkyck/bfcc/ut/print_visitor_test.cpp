#include "highkyck/bfcc/print_visitor.h"

#include <gtest/gtest.h>

#include <string_view>

#include "highkyck/bfcc/lexer.h"
#include "highkyck/bfcc/parser.h"

TEST(PrintVisitor, test_expr) {
  const char* code =
      "test() {abc_123456 = 111; abc_123456; 125 + abc_123456 + (1 - 3 ) * 4 / "
      "2;}";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret,
            "test() {abc_123456 = 111;abc_123456;125 + abc_123456 + 1 - 3"
            " * 4 / 2;}\n");
}

TEST(PrintVisitor, test_relational_operator) {
  const char* code = "test() {a==3;a!=3;a>3;a>=3;a<3;a<=3;}";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, "test() {a == 3;a != 3;a > 3;a >= 3;a < 3;a <= 3;}\n");
}

TEST(PrintVisitor, test_if_else_single_stmt) {
  const char* code = "test() {a=3; if (a!= 3) a=3; else a = a*a;}";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, "test() {a = 3;if (a != 3) a = 3; else a = a * a;}\n");
}

TEST(PrintVisitor, test_if_else_multi_stmt) {
  const char* code =
      "test() {a=3; if (a!= 3) {a=3; a = a+3;} else a = a*a;;a;}";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(
      ret,
      "test() {a = 3;if (a != 3) {a = 3;a = a + 3;} else a = a * a;;a;}\n");
}

TEST(PrintVisitor, test_while_stmt) {
  const char* code = "test() {a=3; while (a <= 10) { a = a + 1; }}";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, "test() {a = 3;while (a <= 10) {a = a + 1;}}\n");
}

TEST(PrintVisitor, test_do_while_stmt) {
  const char* code =
      "test() {a=0; b=1; do {a = a + 1; b = a + b;} while (a < 10);b;}";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(
      ret,
      "test() {a = 0;b = 1;do {a = a + 1;b = a + b;} while (a < 10);b;}\n");
}

TEST(PrintVisitor, test_for_loop_stmt) {
  const char* code = "test() {a=0;b=0;for(a=0;a<=10;a=a+1) {b=b+a;};b;}";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(
      ret,
      "test() {a = 0;b = 0;for (a = 0;a <= 10;a = a + 1) {b = b + a;};b;}\n");
}

TEST(PrintVisitor, test_func_call) {
  const char* code = "sum(n) { a= 0; b = 1; a + b + n;} test() {sum(100);}";
  highkyck::bfcc::Lexer lexer(code);
  lexer.GetNextToken();
  highkyck::bfcc::Parser parser(&lexer);
  highkyck::bfcc::PrintVisitor visitor;
  auto root = parser.Parse();
  root->Accept(&visitor);
  auto ret = visitor.String();
  EXPECT_EQ(ret, "sum(n) {a = 0;b = 1;a + b + n;}test() {sum (100);}\n");
}
