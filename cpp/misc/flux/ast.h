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

#pragma once

#include <ctime>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "token.h"

namespace pl {

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
    Attribute(std::string name, const std::vector<std::shared_ptr<AttributeParam>>& params)
        : name(std::move(name)), params(params) {}
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
    std::unique_ptr<PackageClause> package;
    std::vector<std::shared_ptr<ImportDeclaration>> imports;
    std::vector<std::shared_ptr<Statement>> body;
    std::vector<std::shared_ptr<Comment>> eof;
};

struct PackageClause {
    std::unique_ptr<Identifier> name;
    PackageClause() : name(nullptr) {}
    PackageClause(std::unique_ptr<Identifier> name) : name(std::move(name)) {}
};

struct ImportDeclaration {
    std::unique_ptr<Identifier> alias;
    std::unique_ptr<StringLit> path;
    ImportDeclaration() : alias(nullptr), path(nullptr) {}
    ImportDeclaration(std::unique_ptr<Identifier> alias, std::unique_ptr<StringLit> path)
        : alias(std::move(alias)), path(std::move(path)) {}
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
    VariableAssgn(std::unique_ptr<Identifier> id, std::unique_ptr<Expression> init)
        : id(std::move(id)), init(std::move(init)) {}
    [[nodiscard]] std::string string() const;
};

struct OptionStmt {
    std::unique_ptr<Assignment> assignment;
    OptionStmt() = default;
    OptionStmt(std::unique_ptr<Assignment> assignment) : assignment(std::move(assignment)) {}
    [[nodiscard]] std::string string() const;
};

struct ReturnStmt {
    std::unique_ptr<Expression> argument;
    ReturnStmt() = default;
    ReturnStmt(std::unique_ptr<Expression> argument) : argument(std::move(argument)) {}
    [[nodiscard]] std::string string() const;
};

struct BadStmt {
    std::string text;
    BadStmt(std::string text) : text(std::move(text)) {}
    [[nodiscard]] std::string string() const { return ""; }
};

struct TestCaseStmt {
    std::unique_ptr<Identifier> id;
    std::unique_ptr<StringLit> extends;
    std::unique_ptr<Block> block;
    TestCaseStmt() = default;
    TestCaseStmt(std::unique_ptr<Identifier> id,
                 std::unique_ptr<StringLit> extends,
                 std::unique_ptr<Block> block)
        : id(std::move(id)), extends(std::move(extends)), block(std::move(block)) {}
    [[nodiscard]] std::string string() const { return ""; }
};

struct BuiltinStmt {
    std::vector<std::shared_ptr<Comment>> colon;
    std::unique_ptr<Identifier> id;
    std::unique_ptr<TypeExpression> ty;
    BuiltinStmt() = default;
    BuiltinStmt(const std::vector<std::shared_ptr<Comment>>& colon,
                std::unique_ptr<Identifier> id,
                std::unique_ptr<TypeExpression> ty)
        : colon(colon), id(std::move(id)), ty(std::move(ty)) {}
    [[nodiscard]] std::string string() const { return ""; }
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

    using StmtT = std::variant<std::unique_ptr<ExprStmt>,
                               std::unique_ptr<VariableAssgn>,
                               std::unique_ptr<OptionStmt>,
                               std::unique_ptr<ReturnStmt>,
                               std::unique_ptr<BadStmt>,
                               std::unique_ptr<TestCaseStmt>,
                               std::unique_ptr<BuiltinStmt>>;
    StmtT stmt;
    Statement(Type type, StmtT stmt) : type(type), stmt(std::move(stmt)) {}
    [[nodiscard]] std::string string() const;
};

// expr

struct Identifier {
    std::string name;
    Identifier() = default;
    Identifier(std::string name) : name(std::move(name)) {}
    [[nodiscard]] std::string string() const { return "name: " + name; }
};

struct ArrayItem {
    std::unique_ptr<Expression> expression;
    std::vector<std::shared_ptr<Comment>> comma;
    ArrayItem() = default;
    ArrayItem(std::unique_ptr<Expression> expression,
              const std::vector<std::shared_ptr<Comment>>& comma)
        : expression(std::move(expression)), comma(comma) {}
    [[nodiscard]] std::string string() const;
};

struct ArrayExpr {
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::vector<std::shared_ptr<ArrayItem>> elements;
    std::vector<std::shared_ptr<Comment>> rbrack;
    ArrayExpr() = default;
    ArrayExpr(const std::vector<std::shared_ptr<Comment>>& lbrack,
              const std::vector<std::shared_ptr<ArrayItem>>& elements,
              const std::vector<std::shared_ptr<Comment>>& rbrack)
        : lbrack(lbrack), elements(elements), rbrack(rbrack) {}

    [[nodiscard]] std::string string() const;
};

struct DictItem {
    std::unique_ptr<Expression> key;
    std::unique_ptr<Expression> val;
    std::vector<std::shared_ptr<Comment>> comma;
    DictItem() = default;
    DictItem(std::unique_ptr<Expression> key,
             std::unique_ptr<Expression> val,
             const std::vector<std::shared_ptr<Comment>>& comma)
        : key(std::move(key)), val(std::move(val)), comma(comma) {}
};

struct DictExpr {
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::vector<std::shared_ptr<DictItem>> elements;
    std::vector<std::shared_ptr<Comment>> rbrack;
    DictExpr() = default;
    DictExpr(const std::vector<std::shared_ptr<Comment>>& lbrack,
             const std::vector<std::shared_ptr<DictItem>>& elements,
             const std::vector<std::shared_ptr<Comment>>& rbrack)
        : lbrack(lbrack), elements(elements), rbrack(rbrack) {}
    [[nodiscard]] std::string string() const { return ""; }
};

struct FunctionExpr {
    std::vector<std::shared_ptr<Comment>> lparen;
    std::vector<std::shared_ptr<Property>> params;
    std::vector<std::shared_ptr<Comment>> rparen;
    std::vector<std::shared_ptr<Comment>> arrow;
    std::unique_ptr<FunctionBody> body;

    FunctionExpr() = default;
    FunctionExpr(const std::vector<std::shared_ptr<Comment>>& lparen,
                 const std::vector<std::shared_ptr<Property>>& params,
                 const std::vector<std::shared_ptr<Comment>>& rparen,
                 const std::vector<std::shared_ptr<Comment>>& arrow,
                 std::unique_ptr<FunctionBody> body)
        : lparen(lparen), params(params), rparen(rparen), arrow(arrow), body(std::move(body)) {}
    [[nodiscard]] std::string string() const;
};

struct LogicalExpr {
    LogicalOperator op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    LogicalExpr() = default;
    LogicalExpr(LogicalOperator op,
                std::unique_ptr<Expression> left,
                std::unique_ptr<Expression> right)
        : op(op), left(std::move(left)), right(std::move(right)) {}
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
    ObjectExpr(const std::vector<std::shared_ptr<Comment>>& lbrace,
               std::unique_ptr<WithSource> with,
               const std::vector<std::shared_ptr<Property>>& properties,
               const std::vector<std::shared_ptr<Comment>>& rbrace)
        : lbrace(lbrace), with(std::move(with)), properties(properties), rbrace(rbrace) {}
    [[nodiscard]] std::string string() const;
};

struct MemberExpr {
    std::unique_ptr<Expression> object;
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::unique_ptr<PropertyKey> property;
    std::vector<std::shared_ptr<Comment>> rbrack;
    MemberExpr() = default;
    MemberExpr(std::unique_ptr<Expression> expr,
               const std::vector<std::shared_ptr<Comment>>& lbrack,
               std::unique_ptr<PropertyKey> property,
               const std::vector<std::shared_ptr<Comment>>& rbrack)
        : object(std::move(expr)), lbrack(lbrack), property(std::move(property)), rbrack(rbrack) {}
    [[nodiscard]] std::string string() const;
};

struct IndexExpr {
    std::unique_ptr<Expression> array;
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::unique_ptr<Expression> index;
    std::vector<std::shared_ptr<Comment>> rbrack;

    IndexExpr() = default;
    IndexExpr(std::unique_ptr<Expression> array,
              const std::vector<std::shared_ptr<Comment>>& lbrack,
              std::unique_ptr<Expression> index,
              const std::vector<std::shared_ptr<Comment>>& rbrack)
        : array(std::move(array)), lbrack(lbrack), index(std::move(index)), rbrack(rbrack) {}
    [[nodiscard]] std::string string() const;
};

struct BinaryExpr {
    Operator op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    BinaryExpr() = default;
    BinaryExpr(Operator op, std::unique_ptr<Expression> left, std::unique_ptr<Expression> right)
        : op(op), left(std::move(left)), right(std::move(right)) {}
    [[nodiscard]] std::string string() const;
};

struct UnaryExpr {
    Operator op;
    std::unique_ptr<Expression> argument;
    UnaryExpr() = default;
    UnaryExpr(Operator op, std::unique_ptr<Expression> argument)
        : op(op), argument(std::move(argument)) {}
    [[nodiscard]] std::string string() const;
};

struct PipeExpr {
    std::unique_ptr<Expression> argument;
    std::unique_ptr<CallExpr> call;
    PipeExpr() = default;
    PipeExpr(std::unique_ptr<Expression> argument, std::unique_ptr<CallExpr> call)
        : argument(std::move(argument)), call(std::move(call)) {}
    [[nodiscard]] std::string string() const;
};

struct CallExpr {
    std::unique_ptr<Expression> callee;
    std::vector<std::shared_ptr<Comment>> lparen;
    std::vector<std::shared_ptr<Expression>> arguments;
    std::vector<std::shared_ptr<Comment>> rparen;
    CallExpr() = default;
    CallExpr(std::unique_ptr<Expression> callee,
             const std::vector<std::shared_ptr<Comment>>& lparen,
             const std::vector<std::shared_ptr<Expression>>& arguments,
             const std::vector<std::shared_ptr<Comment>>& rparen)
        : callee(std::move(callee)), lparen(lparen), arguments(arguments), rparen(rparen) {}
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
    StringExpr(std::vector<std::shared_ptr<StringExprPart>> parts) : parts(std::move(parts)) {}
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
    StringExprPart(Type type, StringExprT part) : type(type), part(std::move(part)) {}
    [[nodiscard]] std::string string() const;
};

struct TextPart {
    std::string value;
    [[nodiscard]] std::string string() const;
};

struct InterpolatedPart {
    std::unique_ptr<Expression> expression;
    InterpolatedPart() = default;
    InterpolatedPart(std::unique_ptr<Expression> expression) : expression(std::move(expression)) {}
    [[nodiscard]] std::string string() const;
};

struct ParenExpr {
    std::vector<std::shared_ptr<Comment>> lparen;
    std::unique_ptr<Expression> expression;
    std::vector<std::shared_ptr<Comment>> rparen;
    ParenExpr() = default;
    ParenExpr(const std::vector<std::shared_ptr<Comment>>& lparen,
              std::unique_ptr<Expression> expr,
              const std::vector<std::shared_ptr<Comment>>& rparen)
        : lparen(lparen), expression(std::move(expr)), rparen(rparen) {}
    [[nodiscard]] std::string string() const;
};

struct IntegerLit {
    int64_t value;
    IntegerLit() = default;
    IntegerLit(int64_t value) : value(value) {}
    [[nodiscard]] std::string string() const;
};

struct FloatLit {
    double value;
    FloatLit() = default;
    FloatLit(double value) : value(value) {}
    [[nodiscard]] std::string string() const;
};

struct StringLit {
    std::string value;
    StringLit() = default;
    StringLit(std::string value) : value(std::move(value)) {}
    [[nodiscard]] std::string string() const;
};

struct Duration {
    int64_t magnitude;
    std::string unit;
    Duration(int64_t magnitude, std::string unit) : magnitude(magnitude), unit(std::move(unit)) {}
    [[nodiscard]] std::string string() const;
};

struct DurationLit {
    std::vector<std::shared_ptr<Duration>> values;
    DurationLit() = default;
    DurationLit(const std::vector<std::shared_ptr<Duration>>& values) : values(values) {}
    [[nodiscard]] std::string string() const;
};

struct UintLit {
    uint64_t value;
    UintLit() : value(0) {}
    UintLit(uint64_t value) : value(value) {}
    [[nodiscard]] std::string string() const;
};

struct BooleanLit {
    bool value;
    BooleanLit() : value(false) {}
    BooleanLit(bool value) : value(value) {}
    [[nodiscard]] std::string string() const;
};

struct DateTimeLit {
    std::tm value;
    DateTimeLit() = default;
    DateTimeLit(const std::tm& value) : value(value) {}
    [[nodiscard]] std::string string() const;
};

struct RegexpLit {
    std::string value;
    RegexpLit() = default;
    RegexpLit(std::string value) : value(std::move(value)) {}
    [[nodiscard]] std::string string() const;
};

struct PipeLit {
    [[nodiscard]] std::string string() const;
};

struct LabelLit {
    std::string value;
    LabelLit() = default;
    LabelLit(std::string value) : value(std::move(value)) {}
    [[nodiscard]] std::string string() const;
};

struct BadExpr {
    std::string text;
    std::unique_ptr<Expression> expression;
    BadExpr() = default;
    BadExpr(std::string text, std::unique_ptr<Expression> expression)
        : text(std::move(text)), expression(std::move(expression)) {}
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
    ExprT expr;
    Expression() = default;
    Expression(Type t, ExprT expr) : type(t), expr(std::move(expr)) {}
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
        return "MultiplicationOperator";
    case Operator::DivisionOperator:
        return "DivisionOperator";
    case Operator::ModuloOperator:
        return "ModuloOperator";
    case Operator::PowerOperator:
        return "PowerOperator";
    case Operator::AdditionOperator:
        return "AdditionOperator";
    case Operator::SubtractionOperator:
        return "SubtractionOperator";
    case Operator::LessThanEqualOperator:
        return "LessThanEqualOperator";
    case Operator::LessThanOperator:
        return "LessThanOperator";
    case Operator::GreaterThanEqualOperator:
        return "GreaterThanEqualOperator";
    case Operator::GreaterThanOperator:
        return "GreaterThanOperator";
    case Operator::StartsWithOperator:
        return "StartsWithOperator";
    case Operator::InOperator:
        return "InOperator";
    case Operator::NotOperator:
        return "NotOperator";
    case Operator::ExistsOperator:
        return "ExistsOperator";
    case Operator::NotEmptyOperator:
        return "NotEmptyOperator";
    case Operator::EmptyOperator:
        return "EmptyOperator";
    case Operator::EqualOperator:
        return "EqualOperator";
    case Operator::NotEqualOperator:
        return "NotEqualOperator";
    case Operator::RegexpMatchOperator:
        return "RegexpMatchOperator";
    case Operator::NotRegexpMatchOperator:
        return "NotRegexpMatchOperator";
    case Operator::InvalidOperator:
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
}

// assign
struct MemberAssgn {
    std::unique_ptr<MemberExpr> member;
    std::unique_ptr<Expression> init;
    MemberAssgn() = default;
    MemberAssgn(std::unique_ptr<MemberExpr> member, std::unique_ptr<Expression> init)
        : member(std::move(member)), init(std::move(init)) {}
    [[nodiscard]] std::string string() const;
};

struct Assignment {
    using AssiT = std::variant<std::unique_ptr<VariableAssgn>, std::unique_ptr<MemberAssgn>>;
    enum class Type { VariableAssignment, MemberAssignment };
    Type type;
    AssiT value;

    Assignment() = default;
    Assignment(Assignment::Type type, AssiT value) : type(type), value(std::move(value)) {}
    [[nodiscard]] std::string string() const;
};

// property

struct Property {
    std::unique_ptr<PropertyKey> key;
    std::vector<std::shared_ptr<Comment>> separator;
    std::unique_ptr<Expression> value;
    std::vector<std::shared_ptr<Comment>> comma;
    Property() = default;
    Property(std::unique_ptr<PropertyKey> key,
             const std::vector<std::shared_ptr<Comment>>& separator,
             std::unique_ptr<Expression> value,
             const std::vector<std::shared_ptr<Comment>>& comma)
        : key(std::move(key)), separator(separator), value(std::move(value)), comma(comma) {}
    [[nodiscard]] std::string string() const;
};

struct PropertyKey {
    using PropKeyT = std::variant<std::unique_ptr<Identifier>, std::unique_ptr<StringLit>>;
    enum class Type { Identifier, StringLiteral };

    Type type;
    PropKeyT key;

    PropertyKey() = default;
    PropertyKey(PropertyKey::Type type, PropKeyT key) : type(type), key(std::move(key)) {}
    [[nodiscard]] std::string string() const;
};

// function

struct Block {
    std::vector<std::shared_ptr<Comment>> lbrace;
    std::vector<std::shared_ptr<Statement>> body;
    std::vector<std::shared_ptr<Comment>> rbrace;

    Block() = default;
    Block(const Block&) = default;
    Block(Block&&) = default;
    Block& operator=(const Block&) = default;
    Block& operator=(Block&&) = default;
    Block(const std::vector<std::shared_ptr<Comment>>& lbrace,
          const std::vector<std::shared_ptr<Statement>>& body,
          const std::vector<std::shared_ptr<Comment>>& rbrace)
        : lbrace(lbrace), body(body), rbrace(rbrace) {}
    [[nodiscard]] std::string string() const;
};

struct FunctionBody {
    enum class Type { Block, Expression };
    using FuncT = std::variant<std::unique_ptr<Block>, std::unique_ptr<Expression>>;
    Type type;
    FuncT body;
    FunctionBody(Type t, FuncT body) : type(t), body(std::move(body)) {}
    [[nodiscard]] std::string string() const;
};

// parameter

struct TvarType {
    std::unique_ptr<Identifier> name;
    TvarType() = default;
    TvarType(std::unique_ptr<Identifier> name) : name(std::move(name)) {}
};

struct NamedType {
    std::unique_ptr<Identifier> name;
    NamedType() = default;
    NamedType(std::unique_ptr<Identifier> name) : name(std::move(name)) {}
};

struct ArrayType {
    std::unique_ptr<MonoType> element;
    ArrayType() = default;
    ArrayType(std::unique_ptr<MonoType> element) : element(std::move(element)) {}
};

struct StreamType {
    std::unique_ptr<MonoType> element;
    StreamType() = default;
    StreamType(std::unique_ptr<MonoType> element) : element(std::move(element)) {}
};

struct VectorType {
    std::unique_ptr<MonoType> element;
    VectorType() = default;
    VectorType(std::unique_ptr<MonoType> element) : element(std::move(element)) {}
};

struct DictType {
    std::unique_ptr<MonoType> key;
    std::unique_ptr<MonoType> val;
    DictType() = default;
    DictType(std::unique_ptr<MonoType> key, std::unique_ptr<MonoType> val)
        : key(std::move(key)), val(std::move(val)) {}
};

struct DynamicType {};

struct FunctionType {
    std::vector<std::shared_ptr<ParameterType>> parameters;
    std::unique_ptr<MonoType> monotype;
};

struct RecordType {
    std::unique_ptr<Identifier> tvar;
    std::vector<std::shared_ptr<PropertyType>> properties;
};

struct TypeExpression {
    std::unique_ptr<MonoType> monotype;
    std::vector<std::shared_ptr<TypeConstraint>> constraints;
    TypeExpression() = default;
    TypeExpression(std::unique_ptr<MonoType> monotype,
                   const std::vector<std::shared_ptr<TypeConstraint>>& constraints)
        : monotype(std::move(monotype)), constraints(constraints) {}
};

struct TypeConstraint {
    std::unique_ptr<Identifier> tvar;
    std::vector<std::shared_ptr<Identifier>> kinds;
    TypeConstraint() = default;
    TypeConstraint(std::unique_ptr<Identifier> tvar,
                   const std::vector<std::shared_ptr<Identifier>>& kinds)
        : tvar(std::move(tvar)), kinds(kinds) {}
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
    MonoType(MonoType::Type type, MonoT value) : type(type), value(std::move(value)) {}
};

struct Required {
    std::unique_ptr<Identifier> name;
    std::unique_ptr<MonoType> monotype;
};

struct Optional {
    std::unique_ptr<Identifier> name;
    std::unique_ptr<MonoType> monotype;
    std::unique_ptr<LabelLit> _default;
};

struct Pipe {
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
    ParameterType(ParameterType::Type type, ParamT value) : type(type), value(std::move(value)) {}
};

} // namespace pl
