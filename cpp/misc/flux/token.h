//=====================================================================
//
// token.h -
//
// Created by liubang on 2023/11/01 20:26
// Last Modified: 2023/11/01 20:26
//
//=====================================================================
#pragma once

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
