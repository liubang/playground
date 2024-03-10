// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace pl {

enum class TokenType {
    // ILLEGAL Token, EOF, WS are Special InfluxQL tokens.
    ILLEGAL,
    Eof,
    WS,
    COMMENT,

    literalBeg,
    // IDENT and the following are InfluxQL literal tokens.
    IDENT,       // main
    BOUNDPARAM,  // $param
    NUMBER,      // 12345.67
    INTEGER,     // 12345
    DURATIONVAL, // 13h
    STRING,      // "abc"
    BADSTRING,   // "abc
    BADESCAPE,   // \q
    TRUE,        // true
    FALSE,       // false
    REGEX,       // Regular expressions
    BADREGEX,    // `.*
    literalEnd,

    operatorBeg,
    // ADD and the following are InfluxQL Operators
    ADD,         // +
    SUB,         // -
    MUL,         // *
    DIV,         // /
    MOD,         // %
    BITWISE_AND, // &
    BITWISE_OR,  // |
    BITWISE_XOR, // ^

    AND, // AND
    OR,  // OR

    EQ,       // =
    NEQ,      // !=
    EQREGEX,  // =~
    NEQREGEX, // !~
    LT,       // <
    LTE,      // <=
    GT,       // >
    GTE,      // >=
    operatorEnd,

    LPAREN,      // (
    RPAREN,      // )
    COMMA,       // ,
    COLON,       // :
    DOUBLECOLON, // ::
    SEMICOLON,   // ;
    DOT,         // .

    keywordBeg,
    // ALL and the following are InfluxQL Keywords
    ALL,
    ALTER,
    ANALYZE,
    ANY,
    AS,
    ASC,
    BEGIN,
    BY,
    CARDINALITY,
    CREATE,
    CONTINUOUS,
    DATABASE,
    DATABASES,
    DEFAULT,
    DELETE,
    DESC,
    DESTINATIONS,
    DIAGNOSTICS,
    DISTINCT,
    DROP,
    DURATION,
    END,
    EVERY,
    EXACT,
    EXPLAIN,
    FIELD,
    FOR,
    FROM,
    GRANT,
    GRANTS,
    GROUP,
    GROUPS,
    IN,
    INF,
    INSERT,
    INTO,
    KEY,
    KEYS,
    KILL,
    LIMIT,
    MEASUREMENT,
    MEASUREMENTS,
    NAME,
    OFFSET,
    ON,
    ORDER,
    PASSWORD,
    POLICY,
    POLICIES,
    PRIVILEGES,
    QUERIES,
    QUERY,
    READ,
    REPLICATION,
    RESAMPLE,
    RETENTION,
    REVOKE,
    SELECT,
    SERIES,
    SET,
    SHOW,
    SHARD,
    SHARDS,
    SLIMIT,
    SOFFSET,
    STATS,
    SUBSCRIPTION,
    SUBSCRIPTIONS,
    TAG,
    TO,
    USER,
    USERS,
    VALUES,
    Verbose,
    WHERE,
    WITH,
    WRITE,
    keywordEnd,
};

inline std::string tok_string(TokenType type) {
    switch (type) {
    case TokenType::ILLEGAL:
        return "ILLEGAL";
    case TokenType::Eof:
        return "EOF";
    case TokenType::WS:
        return "WS";
    case TokenType::IDENT:
        return "IDENT";
    case TokenType::NUMBER:
        return "NUMBER";
    case TokenType::DURATIONVAL:
        return "DURATIONVAL";
    case TokenType::STRING:
        return "STRING";
    case TokenType::BADSTRING:
        return "BADSTRING";
    case TokenType::BADESCAPE:
        return "BADESCAPE";
    case TokenType::TRUE:
        return "TRUE";
    case TokenType::FALSE:
        return "FALSE";
    case TokenType::REGEX:
        return "REGEX";
    case TokenType::ADD:
        return "ADD";
    case TokenType::SUB:
        return "SUB";
    case TokenType::MUL:
        return "MUL";
    case TokenType::DIV:
        return "DIV";
    case TokenType::MOD:
        return "MOD";
    case TokenType::BITWISE_AND:
        return "BITWISE_AND";
    case TokenType::BITWISE_OR:
        return "BITWISE_OR";
    case TokenType::BITWISE_XOR:
        return "BITWISE_XOR";
    case TokenType::AND:
        return "AND";
    case TokenType::OR:
        return "OR";
    case TokenType::EQ:
        return "EQ";
    case TokenType::NEQ:
        return "NEQ";
    case TokenType::EQREGEX:
        return "EQREGEX";
    case TokenType::NEQREGEX:
        return "NEQREGEX";
    case TokenType::LT:
        return "LT";
    case TokenType::LTE:
        return "LTE";
    case TokenType::GT:
        return "GT";
    case TokenType::GTE:
        return "GTE";
    case TokenType::LPAREN:
        return "LPAREN";
    case TokenType::RPAREN:
        return "RPAREN";
    case TokenType::COMMA:
        return "COMMA";
    case TokenType::COLON:
        return "COLON";
    case TokenType::DOUBLECOLON:
        return "DOUBLECOLON";
    case TokenType::SEMICOLON:
        return "SEMICOLON";
    case TokenType::DOT:
        return "DOT";
    case TokenType::ALL:
        return "ALL";
    case TokenType::ALTER:
        return "ALTER";
    case TokenType::ANALYZE:
        return "ANALYZE";
    case TokenType::ANY:
        return "ANY";
    case TokenType::AS:
        return "AS";
    case TokenType::ASC:
        return "ASC";
    case TokenType::BEGIN:
        return "BEGIN";
    case TokenType::BY:
        return "BY";
    case TokenType::CARDINALITY:
        return "CARDINALITY";
    case TokenType::CREATE:
        return "CREATE";
    case TokenType::CONTINUOUS:
        return "CONTINUOUS";
    case TokenType::DATABASE:
        return "DATABASE";
    case TokenType::DATABASES:
        return "DATABASES";
    case TokenType::DEFAULT:
        return "DEFAULT";
    case TokenType::DELETE:
        return "DELETE";
    case TokenType::DESC:
        return "DESC";
    case TokenType::DESTINATIONS:
        return "DESTINATIONS";
    case TokenType::DIAGNOSTICS:
        return "DIAGNOSTICS";
    case TokenType::DISTINCT:
        return "DISTINCT";
    case TokenType::DROP:
        return "DROP";
    case TokenType::DURATION:
        return "DURATION";
    case TokenType::END:
        return "END";
    case TokenType::EVERY:
        return "EVERY";
    case TokenType::EXACT:
        return "EXACT";
    case TokenType::EXPLAIN:
        return "EXPLAIN";
    case TokenType::FIELD:
        return "FIELD";
    case TokenType::FOR:
        return "FOR";
    case TokenType::FROM:
        return "FROM";
    case TokenType::GRANT:
        return "GRANT";
    case TokenType::GRANTS:
        return "GRANTS";
    case TokenType::GROUP:
        return "GROUP";
    case TokenType::GROUPS:
        return "GROUPS";
    case TokenType::IN:
        return "IN";
    case TokenType::INF:
        return "INF";
    case TokenType::INSERT:
        return "INSERT";
    case TokenType::INTO:
        return "INTO";
    case TokenType::KEY:
        return "KEY";
    case TokenType::KEYS:
        return "KEYS";
    case TokenType::KILL:
        return "KILL";
    case TokenType::LIMIT:
        return "LIMIT";
    case TokenType::MEASUREMENT:
        return "MEASUREMENT";
    case TokenType::MEASUREMENTS:
        return "MEASUREMENTS";
    case TokenType::NAME:
        return "NAME";
    case TokenType::OFFSET:
        return "OFFSET";
    case TokenType::ON:
        return "ON";
    case TokenType::ORDER:
        return "ORDER";
    case TokenType::PASSWORD:
        return "PASSWORD";
    case TokenType::POLICY:
        return "POLICY";
    case TokenType::POLICIES:
        return "POLICIES";
    case TokenType::PRIVILEGES:
        return "PRIVILEGES";
    case TokenType::QUERIES:
        return "QUERIES";
    case TokenType::QUERY:
        return "QUERY";
    case TokenType::READ:
        return "READ";
    case TokenType::REPLICATION:
        return "REPLICATION";
    case TokenType::RESAMPLE:
        return "RESAMPLE";
    case TokenType::RETENTION:
        return "RETENTION";
    case TokenType::REVOKE:
        return "REVOKE";
    case TokenType::SELECT:
        return "SELECT";
    case TokenType::SERIES:
        return "SERIES";
    case TokenType::SET:
        return "SET";
    case TokenType::SHOW:
        return "SHOW";
    case TokenType::SHARD:
        return "SHARD";
    case TokenType::SHARDS:
        return "SHARDS";
    case TokenType::SLIMIT:
        return "SLIMIT";
    case TokenType::SOFFSET:
        return "SOFFSET";
    case TokenType::STATS:
        return "STATS";
    case TokenType::SUBSCRIPTION:
        return "SUBSCRIPTION";
    case TokenType::SUBSCRIPTIONS:
        return "SUBSCRIPTIONS";
    case TokenType::TAG:
        return "TAG";
    case TokenType::TO:
        return "TO";
    case TokenType::USER:
        return "USER";
    case TokenType::USERS:
        return "USERS";
    case TokenType::VALUES:
        return "VALUES";
    case TokenType::Verbose:
        return "VERBOSE";
    case TokenType::WHERE:
        return "WHERE";
    case TokenType::WITH:
        return "WITH";
    case TokenType::WRITE:
        return "WRITE";
    case TokenType::INTEGER:
        return "INT";
    default:
        return "ILLEGAL";
    }
}

struct Position {
    uint32_t line;
    uint32_t column;

    Position() = default;
    Position(uint32_t line, uint32_t column) : line(line), column(column) {}
    [[nodiscard]] bool is_valid() const { return line > 0 && column > 0; }
    bool operator<(const Position& other) const {
        return line < other.line || (line == other.line && column < other.column);
    }
    static Position invalid() { return {0, 0}; }
};

inline std::ostream& operator<<(std::ostream& os, const Position& pos) {
    os << "{line:" << pos.line << ", column:" << pos.column << "}";
    return os;
}

struct Comment {
    std::string text;
    Comment() = default;
    Comment(std::string text) : text(std::move(text)) {}
};

using CommentRef = std::shared_ptr<Comment>;
using CommentPtr = std::unique_ptr<Comment>;

struct Token {
    TokenType tok;
    std::string lit;
    uint32_t start_offset;
    uint32_t end_offset;
    Position start_pos;
    Position end_pos;
    std::vector<CommentRef> comments;
};

using TokenRef = std::shared_ptr<Token>;
using TokenPtr = std::unique_ptr<Token>;

inline std::ostream& operator<<(std::ostream& os, const Token& token) {
    os << "{tok: " << tok_string(token.tok) << ", lit: " << token.lit << ", offset: ["
       << token.start_offset << ", " << token.end_offset << "], start_pos: " << token.start_pos
       << ", end_pos: " << token.end_pos << "}";
    return os;
}

} // namespace pl
