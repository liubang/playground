#include "highkyck/bfcc/lexer.h"

#include <gtest/gtest.h>

TEST(Lexer, GetNextToken) {
  const char* code = " 125 + 1 - 3 * 4 / 2";
  highkyck::bfcc::Lexer lexer(code);

  // 125
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 125);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "125");

  // +
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Add);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "+");

  // 1
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 1);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "1");

  // -
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Sub);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "-");

  // 3
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 3);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "3");

  // *
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Mul);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "*");

  // 4
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 4);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "4");

  // /
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Div);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "/");

  // 2
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Num);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 2);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "2");

  // Eof
  lexer.GetNextToken();
  EXPECT_EQ(lexer.CurrentToken()->Type(), highkyck::bfcc::TokenType::Eof);
  EXPECT_EQ(lexer.CurrentToken()->Value(), 0);
  EXPECT_EQ(lexer.CurrentToken()->Content(), "");
}
