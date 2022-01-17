#include "highkyck/bfcc/lexer.h"

#include <gtest/gtest.h>

TEST(Lexer, GetNextToken) {
  const char* code = " 125 + 1 - 3 * 4 / 2";
  highkyck::bfcc::Lexer lexer(code);

  // 125
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 125);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "125");

  // +
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Add);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "+");

  // 1
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 1);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "1");

  // -
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Sub);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "-");

  // 3
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 3);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "3");

  // *
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Mul);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "*");

  // 4
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 4);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "4");

  // /
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Div);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "/");

  // 2
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 2);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "2");

  // Eof
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Kind(), highkyck::bfcc::TokenKind::Eof);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "");
}
