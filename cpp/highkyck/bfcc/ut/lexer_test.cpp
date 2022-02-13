#include "highkyck/bfcc/lexer.h"

#include <gtest/gtest.h>

#define TK(t, v, c)                                \
  do {                                             \
    lexer.GetNextToken();                          \
    EXPECT_EQ(lexer.CurrentToken()->Type(), t);    \
    EXPECT_EQ(lexer.CurrentToken()->Value(), v);   \
    EXPECT_EQ(lexer.CurrentToken()->Content(), c); \
  } while (0)

TEST(Lexer, GetNextToken) {
  const char* code = " 125 +abc_124d + (1 - 3 ) * 4 / 2;";
  highkyck::bfcc::Lexer lexer(code);

  // 125
  TK(highkyck::bfcc::TokenType::Num, 125, "125");
  // +
  TK(highkyck::bfcc::TokenType::Add, 0, "+");
  // abc_124d
  TK(highkyck::bfcc::TokenType::Identifier, 0, "abc_124d");
  // +
  TK(highkyck::bfcc::TokenType::Add, 0, "+");
  // (
  TK(highkyck::bfcc::TokenType::LParent, 0, "(");
  // 1
  TK(highkyck::bfcc::TokenType::Num, 1, "1");
  // -
  TK(highkyck::bfcc::TokenType::Sub, 0, "-");
  // 3
  TK(highkyck::bfcc::TokenType::Num, 3, "3");
  // )
  TK(highkyck::bfcc::TokenType::RParent, 0, ")");
  // *
  TK(highkyck::bfcc::TokenType::Mul, 0, "*");
  // 4
  TK(highkyck::bfcc::TokenType::Num, 4, "4");
  // /
  TK(highkyck::bfcc::TokenType::Div, 0, "/");
  // 2
  TK(highkyck::bfcc::TokenType::Num, 2, "2");
  // ;
  TK(highkyck::bfcc::TokenType::Semicolon, 0, ";");
  // Eof
  TK(highkyck::bfcc::TokenType::Eof, 0, "");
}

#undef TK
