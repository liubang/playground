//=====================================================================
//
// token.h -
//
// Created by liubang on 2023/11/01 20:26
// Last Modified: 2023/11/01 20:26
//
//=====================================================================
#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace pl {
enum class TokenType {
    Illegal,
    Eof,
    Comment,

    // Reserved keywords
    And,
    Or,
    Not,
    Import,
    Package,
    Return,
    Option,
    Builtin,
    TestCase,
    If,
    Then,
    Else,

    // Identifiers and literals
    Ident,
    Int,
    Float,
    String,
    Regex,
    Time,
    Duration,

    // Operators
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Pow,
    Eq,
    Lt,
    Gt,
    Lte,
    Gte,
    Neq,
    RegexEq,
    RegexNeq,
    Assign,
    Arrow,
    LParen,
    RParen,
    LBrack,
    RBrack,
    LBrace,
    RBrace,
    Comma,
    Dot,
    Colon,
    PipeForward,
    PipeReceive,
    Exists,

    // String expression tokens
    Quote,
    StringExpr,
    Text,

    QuestionMark,

    Attribute,
};

inline std::string token_to_string(TokenType token) {
    switch (token) {
    case TokenType::Illegal:
        return "Illegal";
    case TokenType::Eof:
        return "Eof";
    case TokenType::Comment:
        return "Comment";
    case TokenType::And:
        return "And";
    case TokenType::Or:
        return "Or";
    case TokenType::Not:
        return "Not";
    case TokenType::Import:
        return "Import";
    case TokenType::Package:
        return "Package";
    case TokenType::Return:
        return "Return";
    case TokenType::Option:
        return "Option";
    case TokenType::Builtin:
        return "Builtin";
    case TokenType::TestCase:
        return "TestCase";
    case TokenType::If:
        return "If";
    case TokenType::Then:
        return "Then";
    case TokenType::Else:
        return "Else";
    case TokenType::Ident:
        return "Ident";
    case TokenType::Int:
        return "Int";
    case TokenType::Float:
        return "Float";
    case TokenType::String:
        return "String";
    case TokenType::Regex:
        return "Regex";
    case TokenType::Time:
        return "Time";
    case TokenType::Duration:
        return "Duration";
    case TokenType::Add:
        return "Add";
    case TokenType::Sub:
        return "Sub";
    case TokenType::Mul:
        return "Mul";
    case TokenType::Div:
        return "Div";
    case TokenType::Mod:
        return "Mod";
    case TokenType::Pow:
        return "Pow";
    case TokenType::Eq:
        return "Eq";
    case TokenType::Lt:
        return "Lt";
    case TokenType::Gt:
        return "Gt";
    case TokenType::Lte:
        return "Lte";
    case TokenType::Gte:
        return "Gte";
    case TokenType::Neq:
        return "Neq";
    case TokenType::RegexEq:
        return "RegexEq";
    case TokenType::RegexNeq:
        return "RegexNeq";
    case TokenType::Assign:
        return "Assign";
    case TokenType::Arrow:
        return "Arrow";
    case TokenType::LParen:
        return "LParen";
    case TokenType::RParen:
        return "RParen";
    case TokenType::LBrack:
        return "LBrack";
    case TokenType::RBrack:
        return "RBrack";
    case TokenType::LBrace:
        return "LBrack";
    case TokenType::RBrace:
        return "RBrace";
    case TokenType::Comma:
        return "Comma";
    case TokenType::Dot:
        return "Dot";
    case TokenType::Colon:
        return "Colon";
    case TokenType::PipeForward:
        return "PipeForward";
    case TokenType::PipeReceive:
        return "PipeReceive";
    case TokenType::Exists:
        return "Exists";
    case TokenType::Quote:
        return "Quote";
    case TokenType::StringExpr:
        return "StringExpr";
    case TokenType::Text:
        return "Text";
    case TokenType::QuestionMark:
        return "QuestionMark";
    case TokenType::Attribute:
        return "Attribute";
    default:
        return "Illegal";
    }
}

struct Position {
    uint32_t line;
    uint32_t column;

    Position() = default;
    Position(uint32_t line, uint32_t column) : line(line), column(column) {}
    bool is_valid() const { return line > 0 && column > 0; }
    bool operator<(const Position& other) const {
        return line < other.line || (line == other.line && column < other.column);
    }
    static Position invalid() { return Position(0, 0); }
};

inline std::ostream& operator<<(std::ostream& os, const Position& pos) {
    os << "{line:" << pos.line << ", column:" << pos.column << "}";
    return os;
}

struct SourceLocation {
    std::string file;
    Position start;
    Position end;
    std::string source;

    SourceLocation() = default;
    SourceLocation(const Position& start, const Position& end)
        : file(""), start(start), end(end), source("") {}
    SourceLocation(std::string file,
                   const Position& start,
                   const Position& end,
                   const std::string& source)
        : file(std::move(file)), start(start), end(end), source(source) {}

    [[nodiscard]] bool is_valid() const { return start.is_valid() && end.is_valid(); }
    static SourceLocation _default() { return {}; }
};

inline std::ostream& operator<<(std::ostream& os, const SourceLocation& loc) {
    os << "file: " << loc.file << "@{start: " << loc.start << ", end: " << loc.end << "}";
    return os;
}

struct Comment {
    std::string text;
    Comment() = default;
    Comment(std::string text) : text(std::move(text)) {}
};

struct Token {
    TokenType tok;
    std::string lit;
    uint32_t start_offset;
    uint32_t end_offset;
    Position start_pos;
    Position end_pos;
    std::vector<std::shared_ptr<Comment>> comments;
};

inline std::ostream& operator<<(std::ostream& os, const Token& token) {
    os << "{tok: " << token_to_string(token.tok) << ", lit: " << token.lit << ", offset: ["
       << token.start_offset << ", " << token.end_offset << "], start_pos: " << token.start_pos
       << ", end_pos: " << token.end_pos << "}";
    return os;
}

} // namespace pl
