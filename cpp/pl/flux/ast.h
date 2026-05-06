// Copyright (c) 2023 The Authors. All rights reserved.
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
// Created: 2023/11/02 16:43

#pragma once

#include "cpp/pl/lang/assume.h"
#include "token.h"
#include <ctime>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace pl::flux {

struct Comment;
struct Attribute;
struct AttributeParam;
struct Package;
struct File;
struct PackageClause;
struct ImportDeclaration;
struct Block;
struct BadStmt;
struct ExprStmt;
struct ReturnStmt;
struct OptionStmt;
struct BuiltinStmt;
struct NamedType;
struct TvarType;
struct ArrayType;
struct StreamType;
struct VectorType;
struct DictType;
struct DynamicType;
struct FunctionType;
struct TypeExpression;
struct TypeConstraint;
struct RecordType;
struct PropertyType;
struct TestCaseStmt;
struct VariableAssgn;
struct MemberAssgn;
struct StringExpr;
struct TextPart;
struct InterpolatedPart;
struct ParenExpr;
struct CallExpr;
struct PipeExpr;
struct MemberExpr;
struct IndexExpr;
struct FunctionExpr;
struct BinaryExpr;
struct UnaryExpr;
struct LogicalExpr;
struct ArrayItem;
struct ArrayExpr;
struct DictExpr;
struct DictItem;
struct WithSource;
struct ObjectExpr;
struct ConditionalExpr;
struct BadExpr;
struct Property;
struct Identifier;
struct PipeLit;
struct StringLit;
struct BooleanLit;
struct FloatLit;
struct IntegerLit;
struct UintLit;
struct LabelLit;
struct RegexpLit;
struct Duration;
struct DurationLit;
struct DateTimeLit;

struct Expression;
struct Statement;
struct Assignment;
struct PropertyKey;
struct FunctionBody;
struct MonoType;
struct ParameterType;
struct StringExprPart;

enum class Operator;
enum class LogicalOperator;

struct AttributeParam {
    std::unique_ptr<Expression> value;
    std::vector<std::shared_ptr<Comment>> comma;
    [[nodiscard]] std::string string() const;
};

struct Attribute {
    std::string name;
    std::vector<std::shared_ptr<AttributeParam>> params;
    Attribute() = default;
    Attribute(std::string name_in, const std::vector<std::shared_ptr<AttributeParam>>& params_in)
        : name(std::move(name_in)), params(params_in) {}
    [[nodiscard]] std::string string() const;
};

struct Package {
    std::string path;
    std::string package;
    std::vector<std::shared_ptr<File>> files;
    std::vector<std::shared_ptr<Statement>> body;
    std::vector<std::shared_ptr<Comment>> eof;
};

struct File {
    std::string name;
    std::string metadata;
    SourceLocation loc;
    std::unique_ptr<PackageClause> package;
    std::vector<std::shared_ptr<ImportDeclaration>> imports;
    std::vector<std::shared_ptr<Statement>> body;
    std::vector<std::shared_ptr<Comment>> eof;
};

struct PackageClause {
    SourceLocation loc;
    std::vector<std::shared_ptr<Attribute>> attributes;
    std::unique_ptr<Identifier> name;
    PackageClause() : name(nullptr) {}
    PackageClause(std::unique_ptr<Identifier> name_in) : name(std::move(name_in)) {}
    [[nodiscard]] std::string string() const;
};

struct ImportDeclaration {
    SourceLocation loc;
    std::vector<std::shared_ptr<Attribute>> attributes;
    std::unique_ptr<Identifier> alias;
    std::unique_ptr<StringLit> path;
    ImportDeclaration() : alias(nullptr), path(nullptr) {}
    ImportDeclaration(std::unique_ptr<Identifier> alias_in, std::unique_ptr<StringLit> path_in)
        : alias(std::move(alias_in)), path(std::move(path_in)) {}
    [[nodiscard]] std::string string() const;
};

// stmt
struct ExprStmt {
    std::unique_ptr<Expression> expression;
    ExprStmt() = default;
    ExprStmt(const ExprStmt&) = delete;
    ExprStmt(ExprStmt&&) = default;
    ExprStmt& operator=(const ExprStmt&) = delete;
    ExprStmt& operator=(ExprStmt&&) = default;
    ExprStmt(std::unique_ptr<Expression> expr) : expression(std::move(expr)) {}
    [[nodiscard]] std::string string() const;
};

struct VariableAssgn {
    std::unique_ptr<Identifier> id;
    std::unique_ptr<Expression> init;
    VariableAssgn() = default;
    VariableAssgn(std::unique_ptr<Identifier> id_in, std::unique_ptr<Expression> init_in)
        : id(std::move(id_in)), init(std::move(init_in)) {}
    [[nodiscard]] std::string string() const;
};

struct OptionStmt {
    std::unique_ptr<Assignment> assignment;
    OptionStmt() = default;
    OptionStmt(std::unique_ptr<Assignment> assignment_in) : assignment(std::move(assignment_in)) {}
    [[nodiscard]] std::string string() const;
};

struct ReturnStmt {
    std::unique_ptr<Expression> argument;
    ReturnStmt() = default;
    ReturnStmt(std::unique_ptr<Expression> argument_in) : argument(std::move(argument_in)) {}
    [[nodiscard]] std::string string() const;
};

struct BadStmt {
    std::string text;
    BadStmt(std::string text_in) : text(std::move(text_in)) {}
    [[nodiscard]] std::string string() const;
};

struct TestCaseStmt {
    std::unique_ptr<Identifier> id;
    std::unique_ptr<StringLit> extends;
    std::unique_ptr<Block> block;
    TestCaseStmt() = default;
    TestCaseStmt(std::unique_ptr<Identifier> id_in,
                 std::unique_ptr<StringLit> extends_in,
                 std::unique_ptr<Block> block_in)
        : id(std::move(id_in)), extends(std::move(extends_in)), block(std::move(block_in)) {}
    [[nodiscard]] std::string string() const;
};

struct BuiltinStmt {
    std::vector<std::shared_ptr<Comment>> colon;
    std::unique_ptr<Identifier> id;
    std::unique_ptr<TypeExpression> ty;
    BuiltinStmt() = default;
    BuiltinStmt(const std::vector<std::shared_ptr<Comment>>& colon_in,
                std::unique_ptr<Identifier> id_in,
                std::unique_ptr<TypeExpression> ty_in)
        : colon(colon_in), id(std::move(id_in)), ty(std::move(ty_in)) {}
    [[nodiscard]] std::string string() const;
};

struct Statement {
    enum class Type {
        ExpressionStatement,
        VariableAssignment,
        OptionStatement,
        ReturnStatement,
        BadStatement,
        TestCaseStatement,
        BuiltinStatement
    };
    Type type;
    SourceLocation loc;
    std::vector<std::shared_ptr<Attribute>> attributes;

    using StmtT = std::variant<std::unique_ptr<ExprStmt>,
                               std::unique_ptr<VariableAssgn>,
                               std::unique_ptr<OptionStmt>,
                               std::unique_ptr<ReturnStmt>,
                               std::unique_ptr<BadStmt>,
                               std::unique_ptr<TestCaseStmt>,
                               std::unique_ptr<BuiltinStmt>>;
    StmtT stmt;
    Statement(Type type_in, StmtT stmt_in) : type(type_in), stmt(std::move(stmt_in)) {}
    [[nodiscard]] std::string string() const;
};

// expr

struct Identifier {
    std::string name;
    Identifier() = default;
    Identifier(std::string name_in) : name(std::move(name_in)) {}
    [[nodiscard]] std::string string() const;
};

struct ArrayItem {
    SourceLocation loc;
    std::unique_ptr<Expression> expression;
    std::vector<std::shared_ptr<Comment>> comma;
    ArrayItem() = default;
    ArrayItem(std::unique_ptr<Expression> expression_in,
              const std::vector<std::shared_ptr<Comment>>& comma_in)
        : expression(std::move(expression_in)), comma(comma_in) {}
    [[nodiscard]] std::string string() const;
};

struct ArrayExpr {
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::vector<std::shared_ptr<ArrayItem>> elements;
    std::vector<std::shared_ptr<Comment>> rbrack;
    ArrayExpr() = default;
    ArrayExpr(const std::vector<std::shared_ptr<Comment>>& lbrack_in,
              const std::vector<std::shared_ptr<ArrayItem>>& elements_in,
              const std::vector<std::shared_ptr<Comment>>& rbrack_in)
        : lbrack(lbrack_in), elements(elements_in), rbrack(rbrack_in) {}

    [[nodiscard]] std::string string() const;
};

struct DictItem {
    SourceLocation loc;
    std::unique_ptr<Expression> key;
    std::unique_ptr<Expression> val;
    std::vector<std::shared_ptr<Comment>> comma;
    DictItem() = default;
    DictItem(std::unique_ptr<Expression> key_in,
             std::unique_ptr<Expression> val_in,
             const std::vector<std::shared_ptr<Comment>>& comma_in)
        : key(std::move(key_in)), val(std::move(val_in)), comma(comma_in) {}
};

struct DictExpr {
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::vector<std::shared_ptr<DictItem>> elements;
    std::vector<std::shared_ptr<Comment>> rbrack;
    DictExpr() = default;
    DictExpr(const std::vector<std::shared_ptr<Comment>>& lbrack_in,
             const std::vector<std::shared_ptr<DictItem>>& elements_in,
             const std::vector<std::shared_ptr<Comment>>& rbrack_in)
        : lbrack(lbrack_in), elements(elements_in), rbrack(rbrack_in) {}
    [[nodiscard]] std::string string() const;
};

struct FunctionExpr {
    std::vector<std::shared_ptr<Comment>> lparen;
    std::vector<std::shared_ptr<Property>> params;
    std::vector<std::shared_ptr<Comment>> rparen;
    std::vector<std::shared_ptr<Comment>> arrow;
    std::unique_ptr<FunctionBody> body;

    FunctionExpr() = default;
    FunctionExpr(const std::vector<std::shared_ptr<Comment>>& lparen_in,
                 const std::vector<std::shared_ptr<Property>>& params_in,
                 const std::vector<std::shared_ptr<Comment>>& rparen_in,
                 const std::vector<std::shared_ptr<Comment>>& arrow_in,
                 std::unique_ptr<FunctionBody> body_in)
        : lparen(lparen_in),
          params(params_in),
          rparen(rparen_in),
          arrow(arrow_in),
          body(std::move(body_in)) {}
    [[nodiscard]] std::string string() const;
};

struct LogicalExpr {
    LogicalOperator op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    LogicalExpr() = default;
    LogicalExpr(LogicalOperator op_in,
                std::unique_ptr<Expression> left_in,
                std::unique_ptr<Expression> right_in)
        : op(op_in), left(std::move(left_in)), right(std::move(right_in)) {}
    [[nodiscard]] std::string string() const;
};

struct WithSource {
    std::unique_ptr<Identifier> source;
    std::vector<std::shared_ptr<Comment>> with;
    [[nodiscard]] std::string string() const;
};

struct ObjectExpr {
    std::vector<std::shared_ptr<Comment>> lbrace;
    std::unique_ptr<WithSource> with;
    std::vector<std::shared_ptr<Property>> properties;
    std::vector<std::shared_ptr<Comment>> rbrace;
    ObjectExpr() = default;
    ObjectExpr(const std::vector<std::shared_ptr<Comment>>& lbrace_in,
               std::unique_ptr<WithSource> with_in,
               const std::vector<std::shared_ptr<Property>>& properties_in,
               const std::vector<std::shared_ptr<Comment>>& rbrace_in)
        : lbrace(lbrace_in),
          with(std::move(with_in)),
          properties(properties_in),
          rbrace(rbrace_in) {}
    [[nodiscard]] std::string string() const;
};

struct MemberExpr {
    std::unique_ptr<Expression> object;
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::unique_ptr<PropertyKey> property;
    std::vector<std::shared_ptr<Comment>> rbrack;
    MemberExpr() = default;
    MemberExpr(std::unique_ptr<Expression> expr_in,
               const std::vector<std::shared_ptr<Comment>>& lbrack_in,
               std::unique_ptr<PropertyKey> property_in,
               const std::vector<std::shared_ptr<Comment>>& rbrack_in)
        : object(std::move(expr_in)),
          lbrack(lbrack_in),
          property(std::move(property_in)),
          rbrack(rbrack_in) {}
    [[nodiscard]] std::string string() const;
};

struct IndexExpr {
    std::unique_ptr<Expression> array;
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::unique_ptr<Expression> index;
    std::vector<std::shared_ptr<Comment>> rbrack;

    IndexExpr() = default;
    IndexExpr(std::unique_ptr<Expression> array_in,
              const std::vector<std::shared_ptr<Comment>>& lbrack_in,
              std::unique_ptr<Expression> index_in,
              const std::vector<std::shared_ptr<Comment>>& rbrack_in)
        : array(std::move(array_in)),
          lbrack(lbrack_in),
          index(std::move(index_in)),
          rbrack(rbrack_in) {}
    [[nodiscard]] std::string string() const;
};

struct BinaryExpr {
    Operator op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    BinaryExpr() = default;
    BinaryExpr(Operator op_in,
               std::unique_ptr<Expression> left_in,
               std::unique_ptr<Expression> right_in)
        : op(op_in), left(std::move(left_in)), right(std::move(right_in)) {}
    [[nodiscard]] std::string string() const;
};

struct UnaryExpr {
    Operator op;
    std::unique_ptr<Expression> argument;
    UnaryExpr() = default;
    UnaryExpr(Operator op_in, std::unique_ptr<Expression> argument_in)
        : op(op_in), argument(std::move(argument_in)) {}
    [[nodiscard]] std::string string() const;
};

struct PipeExpr {
    std::unique_ptr<Expression> argument;
    std::unique_ptr<CallExpr> call;
    PipeExpr() = default;
    PipeExpr(std::unique_ptr<Expression> argument_in, std::unique_ptr<CallExpr> call_in)
        : argument(std::move(argument_in)), call(std::move(call_in)) {}
    [[nodiscard]] std::string string() const;
};

struct CallExpr {
    std::unique_ptr<Expression> callee;
    std::vector<std::shared_ptr<Comment>> lparen;
    std::vector<std::shared_ptr<Expression>> arguments;
    std::vector<std::shared_ptr<Comment>> rparen;
    CallExpr() = default;
    CallExpr(std::unique_ptr<Expression> callee_in,
             const std::vector<std::shared_ptr<Comment>>& lparen_in,
             const std::vector<std::shared_ptr<Expression>>& arguments_in,
             const std::vector<std::shared_ptr<Comment>>& rparen_in)
        : callee(std::move(callee_in)),
          lparen(lparen_in),
          arguments(arguments_in),
          rparen(rparen_in) {}
    [[nodiscard]] std::string string() const;
};

struct ConditionalExpr {
    std::vector<std::shared_ptr<Comment>> tk_if;
    std::unique_ptr<Expression> test;
    std::vector<std::shared_ptr<Comment>> tk_then;
    std::unique_ptr<Expression> consequent;
    std::vector<std::shared_ptr<Comment>> tk_else;
    std::unique_ptr<Expression> alternate;
    [[nodiscard]] std::string string() const;
};

struct StringExpr {
    std::vector<std::shared_ptr<StringExprPart>> parts;
    StringExpr() = default;
    StringExpr(std::vector<std::shared_ptr<StringExprPart>> parts_in)
        : parts(std::move(parts_in)) {}
    [[nodiscard]] std::string string() const;
};

struct StringExprPart {
    enum class Type {
        Text,
        Interpolated,
    };
    using StringExprT = std::variant<std::unique_ptr<TextPart>, std::unique_ptr<InterpolatedPart>>;

    Type type;
    StringExprT part;
    StringExprPart() = default;
    StringExprPart(Type type_in, StringExprT part_in) : type(type_in), part(std::move(part_in)) {}
    [[nodiscard]] std::string string() const;
};

struct TextPart {
    std::string value;
    [[nodiscard]] std::string string() const;
};

struct InterpolatedPart {
    std::unique_ptr<Expression> expression;
    InterpolatedPart() = default;
    InterpolatedPart(std::unique_ptr<Expression> expression_in)
        : expression(std::move(expression_in)) {}
    [[nodiscard]] std::string string() const;
};

struct ParenExpr {
    std::vector<std::shared_ptr<Comment>> lparen;
    std::unique_ptr<Expression> expression;
    std::vector<std::shared_ptr<Comment>> rparen;
    ParenExpr() = default;
    ParenExpr(const std::vector<std::shared_ptr<Comment>>& lparen_in,
              std::unique_ptr<Expression> expr_in,
              const std::vector<std::shared_ptr<Comment>>& rparen_in)
        : lparen(lparen_in), expression(std::move(expr_in)), rparen(rparen_in) {}
    [[nodiscard]] std::string string() const;
};

struct IntegerLit {
    int64_t value;
    IntegerLit() = default;
    IntegerLit(int64_t value_in) : value(value_in) {}
    [[nodiscard]] std::string string() const;
};

struct FloatLit {
    double value;
    FloatLit() = default;
    FloatLit(double value_in) : value(value_in) {}
    [[nodiscard]] std::string string() const;
};

struct StringLit {
    std::string value;
    StringLit() = default;
    StringLit(std::string value_in) : value(std::move(value_in)) {}
    [[nodiscard]] std::string string() const;
};

struct Duration {
    int64_t magnitude;
    std::string unit;
    Duration(int64_t magnitude_in, std::string unit_in)
        : magnitude(magnitude_in), unit(std::move(unit_in)) {}
    [[nodiscard]] std::string string() const;
};

struct DurationLit {
    std::vector<std::shared_ptr<Duration>> values;
    DurationLit() = default;
    DurationLit(const std::vector<std::shared_ptr<Duration>>& values_in) : values(values_in) {}
    [[nodiscard]] std::string string() const;
};

struct UintLit {
    uint64_t value;
    UintLit() : value(0) {}
    UintLit(uint64_t value_in) : value(value_in) {}
    [[nodiscard]] std::string string() const;
};

struct BooleanLit {
    bool value;
    BooleanLit() : value(false) {}
    BooleanLit(bool value_in) : value(value_in) {}
    [[nodiscard]] std::string string() const;
};

struct DateTimeLit {
    std::tm value;
    DateTimeLit() = default;
    DateTimeLit(const std::tm& value_in) : value(value_in) {}
    [[nodiscard]] std::string string() const;
};

struct RegexpLit {
    std::string value;
    RegexpLit() = default;
    RegexpLit(std::string value_in) : value(std::move(value_in)) {}
    [[nodiscard]] std::string string() const;
};

struct PipeLit {
    [[nodiscard]] std::string string() const;
};

struct LabelLit {
    std::string value;
    LabelLit() = default;
    LabelLit(std::string value_in) : value(std::move(value_in)) {}
    [[nodiscard]] std::string string() const;
};

struct BadExpr {
    std::string text;
    std::unique_ptr<Expression> expression;
    BadExpr() = default;
    BadExpr(std::string text_in, std::unique_ptr<Expression> expression_in)
        : text(std::move(text_in)), expression(std::move(expression_in)) {}
    [[nodiscard]] std::string string() const;
};

struct Expression {
    enum class Type {
        Identifier,
        ArrayExpr,
        DictExpr,
        FunctionExpr,
        LogicalExpr,
        ObjectExpr,
        MemberExpr,
        IndexExpr,
        BinaryExpr,
        UnaryExpr,
        PipeExpr,
        CallExpr,
        ConditionalExpr,
        StringExpr,
        ParenExpr,
        IntegerLit,
        FloatLit,
        StringLit,
        DurationLit,
        UnsignedIntegerLit,
        BooleanLit,
        DateTimeLit,
        RegexpLit,
        PipeLit,
        LabelLit,
        BadExpr
    };
    Type type;

    using ExprT = std::variant<std::unique_ptr<Identifier>,
                               std::unique_ptr<ArrayExpr>,
                               std::unique_ptr<DictExpr>,
                               std::unique_ptr<FunctionExpr>,
                               std::unique_ptr<LogicalExpr>,
                               std::unique_ptr<ObjectExpr>,
                               std::unique_ptr<MemberExpr>,
                               std::unique_ptr<IndexExpr>,
                               std::unique_ptr<BinaryExpr>,
                               std::unique_ptr<UnaryExpr>,
                               std::unique_ptr<PipeExpr>,
                               std::unique_ptr<CallExpr>,
                               std::unique_ptr<ConditionalExpr>,
                               std::unique_ptr<StringExpr>,
                               std::unique_ptr<ParenExpr>,
                               std::unique_ptr<IntegerLit>,
                               std::unique_ptr<FloatLit>,
                               std::unique_ptr<StringLit>,
                               std::unique_ptr<DurationLit>,
                               std::unique_ptr<UintLit>,
                               std::unique_ptr<BooleanLit>,
                               std::unique_ptr<DateTimeLit>,
                               std::unique_ptr<RegexpLit>,
                               std::unique_ptr<PipeLit>,
                               std::unique_ptr<LabelLit>,
                               std::unique_ptr<BadExpr>>;
    SourceLocation loc;
    ExprT expr;
    Expression() = default;
    Expression(Type type_in, ExprT expr_in) : type(type_in), expr(std::move(expr_in)) {}
    [[nodiscard]] std::string string() const;
};

// operator
enum class Operator {
    MultiplicationOperator,
    DivisionOperator,
    ModuloOperator,
    PowerOperator,
    AdditionOperator,
    SubtractionOperator,
    LessThanEqualOperator,
    LessThanOperator,
    GreaterThanEqualOperator,
    GreaterThanOperator,
    StartsWithOperator,
    InOperator,
    NotOperator,
    ExistsOperator,
    NotEmptyOperator,
    EmptyOperator,
    EqualOperator,
    NotEqualOperator,
    RegexpMatchOperator,
    NotRegexpMatchOperator,
    InvalidOperator,
};

static std::unordered_map<std::string, Operator> operator_map = {
    {"*", Operator::MultiplicationOperator},
    {"/", Operator::DivisionOperator},
    {"%", Operator::ModuloOperator},
    {"^", Operator::PowerOperator},
    {"+", Operator::AdditionOperator},
    {"-", Operator::SubtractionOperator},
    {"<=", Operator::LessThanEqualOperator},
    {"<", Operator::LessThanOperator},
    {">=", Operator::GreaterThanEqualOperator},
    {">", Operator::GreaterThanOperator},
    {"startswith", Operator::StartsWithOperator},
    {"in", Operator::InOperator},
    {"not", Operator::NotOperator},
    {"exists", Operator::ExistsOperator},
    {"not empty", Operator::NotEmptyOperator},
    {"empty", Operator::EmptyOperator},
    {"==", Operator::EqualOperator},
    {"!=", Operator::NotEqualOperator},
    {"=~", Operator::RegexpMatchOperator},
    {"!~", Operator::NotRegexpMatchOperator},
    {"<INVALID_OP>", Operator::InvalidOperator},
};

inline std::string op_string(Operator op) {
    switch (op) {
        case Operator::MultiplicationOperator:
            return "*";
        case Operator::DivisionOperator:
            return "/";
        case Operator::ModuloOperator:
            return "%";
        case Operator::PowerOperator:
            return "^";
        case Operator::AdditionOperator:
            return "+";
        case Operator::SubtractionOperator:
            return "-";
        case Operator::LessThanEqualOperator:
            return "<=";
        case Operator::LessThanOperator:
            return "<";
        case Operator::GreaterThanEqualOperator:
            return ">=";
        case Operator::GreaterThanOperator:
            return ">";
        case Operator::StartsWithOperator:
            return "startswith";
        case Operator::InOperator:
            return "in";
        case Operator::NotOperator:
            return "not";
        case Operator::ExistsOperator:
            return "exists";
        case Operator::NotEmptyOperator:
            return "not empty";
        case Operator::EmptyOperator:
            return "empty";
        case Operator::EqualOperator:
            return "==";
        case Operator::NotEqualOperator:
            return "!=";
        case Operator::RegexpMatchOperator:
            return "=~";
        case Operator::NotRegexpMatchOperator:
            return "!~";
        case Operator::InvalidOperator:
        default:
            return "InvalidOperator";
    }
}

enum class LogicalOperator {
    AndOperator,
    OrOperator,
};

inline std::string op_string(LogicalOperator op) {
    switch (op) {
        case LogicalOperator::AndOperator:
            return " and ";
        case LogicalOperator::OrOperator:
            return " or ";
    }
    pl::assume_unreachable();
}

// assign
struct MemberAssgn {
    std::unique_ptr<MemberExpr> member;
    std::unique_ptr<Expression> init;
    MemberAssgn() = default;
    MemberAssgn(std::unique_ptr<MemberExpr> member_in, std::unique_ptr<Expression> init_in)
        : member(std::move(member_in)), init(std::move(init_in)) {}
    [[nodiscard]] std::string string() const;
};

struct Assignment {
    using AssiT = std::variant<std::unique_ptr<VariableAssgn>, std::unique_ptr<MemberAssgn>>;
    enum class Type { VariableAssignment, MemberAssignment };
    Type type;
    AssiT value;

    Assignment() = default;
    Assignment(Assignment::Type type_in, AssiT value_in)
        : type(type_in), value(std::move(value_in)) {}
    [[nodiscard]] std::string string() const;
};

// property

struct Property {
    SourceLocation loc;
    std::unique_ptr<PropertyKey> key;
    std::vector<std::shared_ptr<Comment>> separator;
    std::unique_ptr<Expression> value;
    std::vector<std::shared_ptr<Comment>> comma;
    Property() = default;
    Property(std::unique_ptr<PropertyKey> key_in,
             const std::vector<std::shared_ptr<Comment>>& separator_in,
             std::unique_ptr<Expression> value_in,
             const std::vector<std::shared_ptr<Comment>>& comma_in)
        : key(std::move(key_in)),
          separator(separator_in),
          value(std::move(value_in)),
          comma(comma_in) {}
    [[nodiscard]] std::string string() const;
};

struct PropertyKey {
    using PropKeyT = std::variant<std::unique_ptr<Identifier>, std::unique_ptr<StringLit>>;
    enum class Type { Identifier, StringLiteral };

    Type type;
    PropKeyT key;

    PropertyKey() = default;
    PropertyKey(PropertyKey::Type type_in, PropKeyT key_in)
        : type(type_in), key(std::move(key_in)) {}
    [[nodiscard]] std::string string() const;
};

// function

struct Block {
    SourceLocation loc;
    std::vector<std::shared_ptr<Comment>> lbrace;
    std::vector<std::shared_ptr<Statement>> body;
    std::vector<std::shared_ptr<Comment>> rbrace;

    Block() = default;
    Block(const Block&) = default;
    Block(Block&&) = default;
    Block& operator=(const Block&) = default;
    Block& operator=(Block&&) = default;
    Block(const std::vector<std::shared_ptr<Comment>>& lbrace_in,
          const std::vector<std::shared_ptr<Statement>>& body_in,
          const std::vector<std::shared_ptr<Comment>>& rbrace_in)
        : lbrace(lbrace_in), body(body_in), rbrace(rbrace_in) {}
    [[nodiscard]] std::string string() const;
};

struct FunctionBody {
    enum class Type { Block, Expression };
    using FuncT = std::variant<std::unique_ptr<Block>, std::unique_ptr<Expression>>;
    Type type;
    FuncT body;
    FunctionBody(Type type_in, FuncT body_in) : type(type_in), body(std::move(body_in)) {}
    [[nodiscard]] std::string string() const;
};

// parameter

struct TvarType {
    std::unique_ptr<Identifier> name;
    TvarType() = default;
    TvarType(std::unique_ptr<Identifier> name_in) : name(std::move(name_in)) {}
};

struct NamedType {
    std::unique_ptr<Identifier> name;
    NamedType() = default;
    NamedType(std::unique_ptr<Identifier> name_in) : name(std::move(name_in)) {}
};

struct ArrayType {
    std::unique_ptr<MonoType> element;
    ArrayType() = default;
    ArrayType(std::unique_ptr<MonoType> element_in) : element(std::move(element_in)) {}
};

struct StreamType {
    std::unique_ptr<MonoType> element;
    StreamType() = default;
    StreamType(std::unique_ptr<MonoType> element_in) : element(std::move(element_in)) {}
};

struct VectorType {
    std::unique_ptr<MonoType> element;
    VectorType() = default;
    VectorType(std::unique_ptr<MonoType> element_in) : element(std::move(element_in)) {}
};

struct DictType {
    std::unique_ptr<MonoType> key;
    std::unique_ptr<MonoType> val;
    DictType() = default;
    DictType(std::unique_ptr<MonoType> key_in, std::unique_ptr<MonoType> val_in)
        : key(std::move(key_in)), val(std::move(val_in)) {}
};

struct DynamicType {};

struct FunctionType {
    std::vector<std::shared_ptr<ParameterType>> parameters;
    std::unique_ptr<MonoType> monotype;
    FunctionType() = default;
    FunctionType(const std::vector<std::shared_ptr<ParameterType>>& parameters_in,
                 std::unique_ptr<MonoType> monotype_in)
        : parameters(parameters_in), monotype(std::move(monotype_in)) {}
};

struct PropertyType {
    SourceLocation loc;
    std::unique_ptr<Identifier> name;
    std::unique_ptr<MonoType> monotype;
    PropertyType() = default;
    PropertyType(std::unique_ptr<Identifier> name_in, std::unique_ptr<MonoType> monotype_in)
        : name(std::move(name_in)), monotype(std::move(monotype_in)) {}
};

struct RecordType {
    std::unique_ptr<Identifier> tvar;
    std::vector<std::shared_ptr<PropertyType>> properties;
    RecordType() = default;
    RecordType(std::unique_ptr<Identifier> tvar_in,
               const std::vector<std::shared_ptr<PropertyType>>& properties_in)
        : tvar(std::move(tvar_in)), properties(properties_in) {}
};

struct TypeExpression {
    std::unique_ptr<MonoType> monotype;
    std::vector<std::shared_ptr<TypeConstraint>> constraints;
    TypeExpression() = default;
    TypeExpression(std::unique_ptr<MonoType> monotype_in,
                   const std::vector<std::shared_ptr<TypeConstraint>>& constraints_in)
        : monotype(std::move(monotype_in)), constraints(constraints_in) {}
};

struct TypeConstraint {
    SourceLocation loc;
    std::unique_ptr<Identifier> tvar;
    std::vector<std::shared_ptr<Identifier>> kinds;
    TypeConstraint() = default;
    TypeConstraint(std::unique_ptr<Identifier> tvar_in,
                   const std::vector<std::shared_ptr<Identifier>>& kinds_in)
        : tvar(std::move(tvar_in)), kinds(kinds_in) {}
};

struct MonoType {
    enum class Type {
        Tvar,
        Basic,
        Array,
        Stream,
        Vector,
        Dict,
        Dynamic,
        Record,
        Function,
        Label,
    };

    using MonoT = std::variant<std::unique_ptr<TvarType>,
                               std::unique_ptr<NamedType>,
                               std::unique_ptr<ArrayType>,
                               std::unique_ptr<StreamType>,
                               std::unique_ptr<VectorType>,
                               std::unique_ptr<DictType>,
                               std::unique_ptr<DynamicType>,
                               std::unique_ptr<RecordType>,
                               std::unique_ptr<FunctionType>,
                               std::unique_ptr<LabelLit>>;
    Type type;
    MonoT value;

    MonoType() = default;
    MonoType(MonoType::Type type_in, MonoT value_in) : type(type_in), value(std::move(value_in)) {}
};

struct Required {
    SourceLocation loc;
    std::unique_ptr<Identifier> name;
    std::unique_ptr<MonoType> monotype;
};

struct Optional {
    SourceLocation loc;
    std::unique_ptr<Identifier> name;
    std::unique_ptr<MonoType> monotype;
    std::unique_ptr<LabelLit> _default;
};

struct Pipe {
    SourceLocation loc;
    std::unique_ptr<Identifier> name;
    std::unique_ptr<MonoType> monotype;
};

struct ParameterType {
    enum class Type {
        Required,
        Optional,
        Pipe,
    };

    using ParamT =
        std::variant<std::shared_ptr<Required>, std::unique_ptr<Optional>, std::unique_ptr<Pipe>>;

    Type type;
    ParamT value;

    ParameterType() = default;
    ParameterType(ParameterType::Type type_in, ParamT value_in)
        : type(type_in), value(std::move(value_in)) {}
};

} // namespace pl::flux
