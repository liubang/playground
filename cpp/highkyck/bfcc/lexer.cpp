#include "lexer.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "diagnostic.h"

namespace {

/* clang-format off */
constexpr char BFCC_CHAR_EOF       = '\0';
constexpr char BFCC_CHAR_ADD       = '+';
constexpr char BFCC_CHAR_SUB       = '-';
constexpr char BFCC_CHAR_MUL       = '*';
constexpr char BFCC_CHAR_DIV       = '/';
constexpr char BFCC_CHAR_LPARENT   = '(';
constexpr char BFCC_CHAR_RPARENT   = ')';
constexpr char BFCC_CHAR_COMMA     = ',';
constexpr char BFCC_CHAR_SEMICOLON = ';';
constexpr char BFCC_CHAR_ASSIGN    = '=';
constexpr char BFCC_CHAR_NOT       = '!';
constexpr char BFCC_CHAR_GREATER   = '>';
constexpr char BFCC_CHAR_LESSER    = '<';
constexpr char BFCC_CHAR_LBRACE    = '{';
constexpr char BFCC_CHAR_RBRACE    = '}';

// some ids
constexpr std::string_view BFCC_ID_IF     = "if";
constexpr std::string_view BFCC_ID_ELSE   = "else";
constexpr std::string_view BFCC_ID_WHILE  = "while";
constexpr std::string_view BFCC_ID_DO     = "do";
constexpr std::string_view BFCC_ID_FOR    = "for";
constexpr std::string_view BFCC_ID_RETURN = "return";
/* clang-format on */

}// namespace

namespace highkyck::bfcc {

void Lexer::GetNextChar()
{
  if (cursor_ == source_code_.size()) {
    cur_char_ = '\0';
    cursor_++;
  } else {
    assert(cursor_ < source_code_.size());
    cur_char_ = source_code_[cursor_++];
  }
}

void Lexer::BeginPeekToken()
{
  peek_cur_char_ = cur_char_;
  peek_cur_cursor_ = cursor_;
  peek_cur_line_ = line_;
  peek_cur_line_head_ = line_head_;
  peek_cur_token_ = cur_token_;
}

void Lexer::EndPeekToken()
{
  cur_char_ = peek_cur_char_;
  cursor_ = peek_cur_cursor_;
  line_ = peek_cur_line_;
  line_head_ = peek_cur_line_head_;
  cur_token_ = peek_cur_token_;
}

void Lexer::ExpectToken(TokenType type)
{
  if (CurrentToken()->type == type) {
    GetNextToken();
  } else {
    DiagnosticError(source_code_, line_, CurrentToken()->location.col, "'%s' expected", TokenTypeName(type).data());
  }
}

char Lexer::PeekChar(int distance)
{
  assert(distance >= 0);
  std::size_t idx = cursor_ - 1 + distance;
  if (idx < source_code_.size()) {
    return source_code_[idx];
  } else {
    return '\0';
  }
}

bool Lexer::IsLetter() { return std::isalpha(cur_char_) || cur_char_ == '_'; }

bool Lexer::IsDigit() { return std::isdigit(cur_char_); }

bool Lexer::IsLetterOrDigit() { return IsLetter() || IsDigit(); }

void Lexer::GetNextToken()
{
  // sksip white space
  while (::isspace(cur_char_)) {
    if (cur_char_ == '\n') {
      line_++;
      line_head_ = cursor_;
    }
    GetNextChar();
  }

  TokenType kind;
  SourceLocation location = { .line = line_, .col = cursor_ - 1 - line_head_ };
  int64_t value = 0;
  int64_t start_pos = cursor_ - 1;

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
  case BFCC_CHAR_LBRACE:
    kind = TokenType::LBrace;
    GetNextChar();
    break;
  case BFCC_CHAR_RBRACE:
    kind = TokenType::RBrace;
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
  case BFCC_CHAR_COMMA:
    kind = TokenType::Comma;
    GetNextChar();
    break;
  case BFCC_CHAR_SEMICOLON:
    kind = TokenType::Semicolon;
    GetNextChar();
    break;
  case BFCC_CHAR_ASSIGN:
    if (PeekChar(1) == BFCC_CHAR_ASSIGN) {
      GetNextChar();
      kind = TokenType::Equal;
    } else {
      kind = TokenType::Assign;
    }
    GetNextChar();
    break;
  case BFCC_CHAR_NOT:
    if (PeekChar(1) == BFCC_CHAR_ASSIGN) {
      GetNextChar();
      kind = TokenType::PipeEqual;
    } else {
      DiagnosticError(source_code_, location.line, location.col, "current '%c' is illegal", cur_char_);
    }
    GetNextChar();
    break;
  case BFCC_CHAR_GREATER:
    if (PeekChar(1) == BFCC_CHAR_ASSIGN) {
      GetNextChar();
      kind = TokenType::GreaterEqual;
    } else {
      kind = TokenType::Greater;
    }
    GetNextChar();
    break;
  case BFCC_CHAR_LESSER:
    if (PeekChar(1) == BFCC_CHAR_ASSIGN) {
      GetNextChar();
      kind = TokenType::LesserEqual;
    } else {
      kind = TokenType::Lesser;
    }
    GetNextChar();
    break;
  default:
    if (IsLetter()) {
      while (IsLetterOrDigit()) { GetNextChar(); }
      std::string_view content = source_code_.substr(start_pos, cursor_ - 1 - start_pos);
      if (content == BFCC_ID_IF) {
        kind = TokenType::If;
      } else if (content == BFCC_ID_ELSE) {
        kind = TokenType::Else;
      } else if (content == BFCC_ID_WHILE) {
        kind = TokenType::While;
      } else if (content == BFCC_ID_DO) {
        kind = TokenType::Do;
      } else if (content == BFCC_ID_FOR) {
        kind = TokenType::For;
      } else if (content == BFCC_ID_RETURN) {
        kind = TokenType::Return;
      } else {
        kind = TokenType::Identifier;
      }
    } else if (IsDigit()) {
      do {
        value = value * 10 + cur_char_ - '0';
        GetNextChar();
      } while (IsDigit());
      kind = TokenType::Num;
    } else {
      DiagnosticError(source_code_, location.line, location.col, "current '%c' is illegal", cur_char_);
    }
    break;
  }

  cur_token_ = std::make_shared<Token>(kind, value, source_code_.substr(start_pos, cursor_ - 1 - start_pos), location);
}

}// namespace highkyck::bfcc
