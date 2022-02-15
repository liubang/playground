#include "highkyck/bfcc/lexer.h"

#include <gtest/gtest.h>

#define TK(t, v, c)                              \
  do {                                           \
    lexer.GetNextToken();                        \
    EXPECT_EQ(lexer.CurrentToken()->type, t);    \
    EXPECT_EQ(lexer.CurrentToken()->value, v);   \
    EXPECT_EQ(lexer.CurrentToken()->content, c); \
  } while (0)

TEST(Lexer, GetNextToken) {
  constexpr char code[] =
      " 125 +abc_124d + (1 - 3 ) * 4 / 2;a == 1;a > 1; a>=1; a < 1; a<=1;";

  highkyck::bfcc::Lexer lexer(code);

  TK(highkyck::bfcc::TokenType::Num, 125, "125");
  TK(highkyck::bfcc::TokenType::Add, 0, "+");
  TK(highkyck::bfcc::TokenType::Identifier, 0, "abc_124d");
  TK(highkyck::bfcc::TokenType::Add, 0, "+");
  TK(highkyck::bfcc::TokenType::LParent, 0, "(");
  TK(highkyck::bfcc::TokenType::Num, 1, "1");
  TK(highkyck::bfcc::TokenType::Sub, 0, "-");
  TK(highkyck::bfcc::TokenType::Num, 3, "3");
  TK(highkyck::bfcc::TokenType::RParent, 0, ")");
  TK(highkyck::bfcc::TokenType::Mul, 0, "*");
  TK(highkyck::bfcc::TokenType::Num, 4, "4");
  TK(highkyck::bfcc::TokenType::Div, 0, "/");
  TK(highkyck::bfcc::TokenType::Num, 2, "2");
  TK(highkyck::bfcc::TokenType::Semicolon, 0, ";");

  TK(highkyck::bfcc::TokenType::Identifier, 0, "a");
  TK(highkyck::bfcc::TokenType::Equal, 0, "==");
  TK(highkyck::bfcc::TokenType::Num, 1, "1");
  TK(highkyck::bfcc::TokenType::Semicolon, 0, ";");

  TK(highkyck::bfcc::TokenType::Identifier, 0, "a");
  TK(highkyck::bfcc::TokenType::Greater, 0, ">");
  TK(highkyck::bfcc::TokenType::Num, 1, "1");
  TK(highkyck::bfcc::TokenType::Semicolon, 0, ";");

  TK(highkyck::bfcc::TokenType::Identifier, 0, "a");
  TK(highkyck::bfcc::TokenType::GreaterEqual, 0, ">=");
  TK(highkyck::bfcc::TokenType::Num, 1, "1");
  TK(highkyck::bfcc::TokenType::Semicolon, 0, ";");

  TK(highkyck::bfcc::TokenType::Identifier, 0, "a");
  TK(highkyck::bfcc::TokenType::Lesser, 0, "<");
  TK(highkyck::bfcc::TokenType::Num, 1, "1");
  TK(highkyck::bfcc::TokenType::Semicolon, 0, ";");

  TK(highkyck::bfcc::TokenType::Identifier, 0, "a");
  TK(highkyck::bfcc::TokenType::LesserEqual, 0, "<=");
  TK(highkyck::bfcc::TokenType::Num, 1, "1");
  TK(highkyck::bfcc::TokenType::Semicolon, 0, ";");

  // Eof
  TK(highkyck::bfcc::TokenType::Eof, 0, "");
}

#undef TK
