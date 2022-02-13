#include "lexer.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr char BFCC_CHAR_EOF = '\0';
constexpr char BFCC_CHAR_ADD = '+';
constexpr char BFCC_CHAR_SUB = '-';
constexpr char BFCC_CHAR_MUL = '*';
constexpr char BFCC_CHAR_DIV = '/';
constexpr char BFCC_CHAR_LPARENT = '(';
constexpr char BFCC_CHAR_RPARENT = ')';
constexpr char BFCC_CHAR_SEMICOLON = ';';
constexpr char BFCC_CHAR_ASSIGN = '=';

}  // namespace

namespace highkyck {
namespace bfcc {

void Lexer::GetNextChar() {
  if (cursor_ == source_code_.size()) {
    cur_char_ = '\0';
    cursor_++;
  } else {
    assert(cursor_ < source_code_.size());
    cur_char_ = source_code_[cursor_++];
  }
}

bool Lexer::IsLetter() { return std::isalpha(cur_char_) || cur_char_ == '_'; }

bool Lexer::IsDigit() { return std::isdigit(cur_char_); }

bool Lexer::IsLetterOrDigit() { return IsLetter() || IsDigit(); }

void Lexer::GetNextToken() {
  // sksip white space
  while (::isspace(cur_char_)) {
    GetNextChar();
  }
  TokenType kind;
  int value = 0;
  int start_pos = cursor_ - 1;
  switch (cur_char_) {
    case BFCC_CHAR_EOF:
      kind = TokenType::Eof;
      break;
    case BFCC_CHAR_ADD:
      kind = TokenType::Add;
      GetNextChar();
      break;
    case BFCC_CHAR_SUB:
      kind = TokenType::Sub;
      GetNextChar();
      break;
    case BFCC_CHAR_MUL:
      kind = TokenType::Mul;
      GetNextChar();
      break;
    case BFCC_CHAR_DIV:
      kind = TokenType::Div;
      GetNextChar();
      break;
    case BFCC_CHAR_LPARENT:
      kind = TokenType::LParent;
      GetNextChar();
      break;
    case BFCC_CHAR_RPARENT:
      kind = TokenType::RParent;
      GetNextChar();
      break;
    case BFCC_CHAR_SEMICOLON:
      kind = TokenType::Semicolon;
      GetNextChar();
      break;
    case BFCC_CHAR_ASSIGN:
      kind = TokenType::Assign;
      GetNextChar();
      break;
    default:
      if (IsLetter()) {
        while (IsLetterOrDigit()) {
          GetNextChar();
        }
        kind = TokenType::Identifier;
      } else if (IsDigit()) {
        do {
          value = value * 10 + cur_char_ - '0';
          GetNextChar();
        } while (IsDigit());
        kind = TokenType::Num;
      } else {
        ::printf("not supported %c\n", cur_char_);
        assert(0);
      }
      break;
  }
  cur_token_ = std::make_shared<Token>(
      kind, value, source_code_.substr(start_pos, cursor_ - 1 - start_pos));
}

}  // namespace bfcc
}  // namespace highkyck
