#pragma once

#include <memory>
#include <string_view>

namespace highkyck {
namespace bfcc {

enum class TokenType {
  Add,         // +
  Sub,         // -
  Mul,         // *
  Div,         // /
  Num,         // number
  LParent,     // (
  RParent,     // )
  Identifier,  // variable
  Semicolon,   // ;
  Assign,      // =
  Eof,
};

class Token {
 public:
  Token(TokenType type, int value, std::string_view content)
      : type_(type), value_(value), content_(content) {}

  TokenType Type() const { return type_; }
  int Value() const { return value_; }
  std::string_view Content() const { return content_; }

 private:
  TokenType type_;
  int value_;
  std::string_view content_;
};

class Lexer {
 public:
  Lexer(const char* code) : source_code_(code) {}
  void GetNextToken();
  std::shared_ptr<Token> CurrentToken() const { return cur_token_; }

 private:
  void GetNextChar();
  bool IsLetter();
  bool IsDigit();
  bool IsLetterOrDigit();

 private:
  std::string_view source_code_;
  std::shared_ptr<Token> cur_token_;
  char cur_char_{' '};
  int cursor_{0};
};

}  // namespace bfcc
}  // namespace highkyck
