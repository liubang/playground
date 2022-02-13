#pragma once

#include <memory>
#include <ostream>
#include <string_view>

namespace highkyck {
namespace bfcc {

enum class TokenType {
  Add,           // +
  Sub,           // -
  Mul,           // *
  Div,           // /
  Num,           // number
  LParent,       // (
  RParent,       // )
  Identifier,    // variable
  Semicolon,     // ;
  Assign,        // =
  Equal,         // ==
  PipeEqual,     // !=
  Greater,       // >
  GreaterEqual,  // >=
  Lesser,        // <
  LesserEqual,   // <=
  Eof,
};

inline std::string TokenTypeName(TokenType type) {
  switch (type) {
    case TokenType::Add:
      return "+";
    case TokenType::Sub:
      return "-";
    case TokenType::Mul:
      return "*";
    case TokenType::Div:
      return "/";
    case TokenType::Num:
      return "Number";
    case TokenType::LParent:
      return "(";
    case TokenType::RParent:
      return ")";
    case TokenType::Identifier:
      return "Identifier";
    case TokenType::Semicolon:
      return ";";
    case TokenType::Assign:
      return "=";
    case TokenType::Equal:
      return "==";
    case TokenType::PipeEqual:
      return "!=";
    case TokenType::Greater:
      return ">";
    case TokenType::GreaterEqual:
      return ">=";
    case TokenType::Lesser:
      return "<";
    case TokenType::LesserEqual:
      return "<=";
    case TokenType::Eof:
      return "Eof";
    default:
      return "Unknown";
  }
}

struct SourceLocation {
  int64_t line;
  int64_t col;
};

inline std::ostream& operator<<(std::ostream& os,
                                const SourceLocation& location) {
  os << "(" << location.line << ", " << location.col << ")";
  return os;
}

class Token {
 public:
  Token(TokenType type, int value, std::string_view content,
        const SourceLocation& location)
      : type_(type), value_(value), content_(content), location_(location) {}

  TokenType Type() const { return type_; }
  int Value() const { return value_; }
  std::string_view Content() const { return content_; }
  const SourceLocation& Location() const { return location_; }

 private:
  TokenType type_;
  int value_;
  std::string_view content_;
  SourceLocation location_;
};

inline std::ostream& operator<<(std::ostream& os, const Token& token) {
  os << "Token{Content: " << token.Content()
     << ", Type: " << TokenTypeName(token.Type())
     << ", Location: " << token.Location() << "}";
  return os;
}

class Lexer {
 public:
  Lexer(const char* code) : source_code_(code) {}
  void GetNextToken();
  void ExpectToken(TokenType type);
  std::shared_ptr<Token> CurrentToken() const { return cur_token_; }
  std::string_view SourceCode() const { return source_code_; }

 private:
  void GetNextChar();
  bool IsLetter();
  bool IsDigit();
  bool IsLetterOrDigit();
  char PeekChar(int distance);

 private:
  std::string_view source_code_;
  std::shared_ptr<Token> cur_token_;
  char cur_char_{' '};
  int64_t cursor_{0};
  int64_t line_{0};
  int64_t line_head_{0};
};

}  // namespace bfcc
}  // namespace highkyck
