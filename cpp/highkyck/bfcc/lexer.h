#pragma once

#include <memory>
#include <string_view>

namespace highkyck {
namespace bfcc {

enum class TokenKind {
  Add,
  Sub,
  Mul,
  Div,
  Num,
  Eof,
};

class Token {
 public:
  Token(TokenKind kind, int value, const std::string_view& content)
      : kind_(kind), value_(value), content_(content) {}

  TokenKind Kind() const { return kind_; }
  int Value() const { return value_; }
  const std::string_view& Content() const { return content_; }

 private:
  TokenKind kind_;
  int value_;
  std::string_view content_;
};

class Lexer {
 public:
  Lexer(const char* code) : source_code_(code) {}
  void GetNextToken();
  void GetNextChar();
  std::shared_ptr<Token> CurrentToken() const { return cur_token_; }

 private:
  std::string_view source_code_;
  std::shared_ptr<Token> cur_token_;
  char cur_char_{' '};
  int cursor_{0};
};

}  // namespace bfcc
}  // namespace highkyck
