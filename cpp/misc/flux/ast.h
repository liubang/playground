//=====================================================================
//
// ast.h -
//
// Created by liubang on 2023/11/02 15:00
// Last Modified: 2023/11/02 15:00
//
//=====================================================================
#pragma once

#include <chrono>
#include <unordered_map>
#include <variant>
#include <vector>

#include "token.h"

namespace pl {

struct Position;
struct SourceLocation;
struct Comment;
struct BaseNode;
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
struct Operator;

enum class LogicalOperator;

struct BaseNode {
    // implementation details
    SourceLocation location;
    std::vector<std::shared_ptr<Comment>> comments;
    std::vector<std::shared_ptr<Attribute>> attributes;
    std::vector<std::string> errors;
};

struct AttributeParam : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Expression> value;
    std::vector<std::shared_ptr<Comment>> comma;
};

struct Attribute : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string name;
    std::vector<std::shared_ptr<AttributeParam>> params;
};

struct Package : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string path;
    std::string package;
    std::vector<std::shared_ptr<File>> files;
    std::vector<std::shared_ptr<Statement>> body;
    std::vector<std::shared_ptr<Comment>> eof;
};

struct File : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string name;
    std::string metadata;
    std::shared_ptr<PackageClause> package;
    std::vector<std::shared_ptr<ImportDeclaration>> imports;
    std::vector<std::shared_ptr<Statement>> body;
    std::vector<std::shared_ptr<Comment>> eof;
};

struct PackageClause : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> name;
};

struct ImportDeclaration : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> alias;
    std::shared_ptr<StringLit> path;
};

// stmt

struct ExprStmt : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Expression> expression;
};

struct VariableAssgn : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> id;
    std::shared_ptr<Expression> init;
};

struct OptionStmt : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Assignment> assignment;
};

struct ReturnStmt {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Expression> argument;
};

struct BadStmt : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string text;
};

struct TestCaseStmt : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> id;
    std::shared_ptr<StringLit> extends;
    std::shared_ptr<Block> block;
};

struct BuiltinStmt : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<Comment>> colon;
    std::shared_ptr<Identifier> id;
    std::shared_ptr<TypeExpression> ty;
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

    std::variant<std::shared_ptr<ExprStmt>,
                 std::shared_ptr<VariableAssgn>,
                 std::shared_ptr<OptionStmt>,
                 std::shared_ptr<ReturnStmt>,
                 std::shared_ptr<BadStmt>,
                 std::shared_ptr<TestCaseStmt>,
                 std::shared_ptr<BuiltinStmt>>
        stmt;

    // Statement(Type t, const std::shared_ptr<BaseNode>& stmt) : type(t) {
    //     switch (t) {
    //     case Type::ExpressionStatement:
    //         this->stmt = std::static_pointer_cast<ExprStmt>(stmt);
    //         break;
    //     case Type::VariableAssignment:
    //         this->stmt = std::static_pointer_cast<VariableAssgn>(stmt);
    //         break;
    //     case Type::OptionStatement:
    //         this->stmt = std::static_pointer_cast<OptionStmt>(stmt);
    //         break;
    //     case Type::ReturnStatement:
    //         this->stmt = std::static_pointer_cast<ReturnStmt>(stmt);
    //         break;
    //     case Type::BadStatement:
    //         this->stmt = std::static_pointer_cast<BadStmt>(stmt);
    //         break;
    //     case Type::TestCaseStatement:
    //         this->stmt = std::static_pointer_cast<TestCaseStmt>(stmt);
    //         break;
    //     case Type::BuiltinStatement:
    //         this->stmt = std::static_pointer_cast<BuiltinStmt>(stmt);
    //         break;
    //     }
    // }

    std::shared_ptr<BaseNode> base() {
        switch (type) {
        case Type::ExpressionStatement:
            return std::get<std::shared_ptr<ExprStmt>>(stmt)->base;
        case Type::VariableAssignment:
            return std::get<std::shared_ptr<VariableAssgn>>(stmt)->base;
        case Type::OptionStatement:
            return std::get<std::shared_ptr<OptionStmt>>(stmt)->base;
        case Type::ReturnStatement:
            return std::get<std::shared_ptr<ReturnStmt>>(stmt)->base;
        case Type::BadStatement:
            return std::get<std::shared_ptr<BadStmt>>(stmt)->base;
        case Type::TestCaseStatement:
            return std::get<std::shared_ptr<TestCaseStmt>>(stmt)->base;
        case Type::BuiltinStatement:
            return std::get<std::shared_ptr<BuiltinStmt>>(stmt)->base;
        }
    }
};

// expr

struct Identifier : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string name;
};

struct ArrayExpr {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::vector<std::shared_ptr<ArrayItem>> elements;
    std::vector<std::shared_ptr<Comment>> rbrack;
};

struct DictExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::vector<std::shared_ptr<DictItem>> elements;
    std::vector<std::shared_ptr<Comment>> rbrack;
};

struct FunctionExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<Comment>> lparen;
    std::vector<std::shared_ptr<Property>> params;
    std::vector<std::shared_ptr<Comment>> rparen;
    std::vector<std::shared_ptr<Comment>> arrow;
    std::shared_ptr<FunctionBody> body;
};

struct LogicalExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    LogicalOperator op;
    std::shared_ptr<Expression> left;
    std::shared_ptr<Expression> right;
};

struct ObjectExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<Comment>> lbrace;
    std::shared_ptr<WithSource> with;
    std::vector<std::shared_ptr<Property>> properties;
    std::vector<std::shared_ptr<Comment>> rbrace;
};

struct MemberExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Expression> object;
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::shared_ptr<PropertyKey> property;
    std::vector<std::shared_ptr<Comment>> rbrack;
};

struct IndexExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Expression> array;
    std::vector<std::shared_ptr<Comment>> lbrack;
    std::shared_ptr<Expression> index;
    std::vector<std::shared_ptr<Comment>> rbrack;
};

struct BinaryExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Operator> op;
    std::shared_ptr<Expression> left;
    std::shared_ptr<Expression> right;
};

struct UnaryExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Operator> op;
    std::shared_ptr<Expression> argument;
};

struct PipeExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Expression> argument;
    std::shared_ptr<CallExpr> call;
};

struct CallExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Expression> callee;
    std::vector<std::shared_ptr<Comment>> lparen;
    std::vector<std::shared_ptr<Expression>> arguments;
    std::vector<std::shared_ptr<Comment>> rparen;
};

struct ConditionalExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<Comment>> tk_if;
    std::shared_ptr<Expression> test;
    std::vector<std::shared_ptr<Comment>> tk_then;
    std::shared_ptr<Expression> consequent;
    std::vector<std::shared_ptr<Comment>> tk_else;
    std::shared_ptr<Expression> alternate;
};

struct StringExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<StringExprPart>> parts;
};

struct StringExprPart {
    enum class Type {
        Text,
        Interpolated,
    };
    Type type;
    std::shared_ptr<TextPart> text;
    std::shared_ptr<InterpolatedPart> interpolated;
};

struct TextPart : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string value;
};

struct InterpolatedPart : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Expression> expression;
};

struct ParenExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<Comment>> lparen;
    std::shared_ptr<Expression> expression;
    std::vector<std::shared_ptr<Comment>> rparen;
};

struct IntegerLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
    int64_t value;
};

struct FloatLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
    double value;
};

struct StringLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string value;
};

struct DurationLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<Duration>> values;
};

struct UintLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
    uint64_t value;
};

struct BooleanLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
    bool value;
};

struct DateTimeLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::chrono::system_clock::time_point value;
};

struct RegexpLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string value;
};

struct PipeLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
};

struct LabelLit : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string value;
};

struct BadExpr : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::string text;
    std::shared_ptr<Expression> expression;
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

    std::variant<std::shared_ptr<Identifier>,
                 std::shared_ptr<ArrayExpr>,
                 std::shared_ptr<DictExpr>,
                 std::shared_ptr<FunctionExpr>,
                 std::shared_ptr<LogicalExpr>,
                 std::shared_ptr<ObjectExpr>,
                 std::shared_ptr<MemberExpr>,
                 std::shared_ptr<IndexExpr>,
                 std::shared_ptr<BinaryExpr>,
                 std::shared_ptr<UnaryExpr>,
                 std::shared_ptr<PipeExpr>,
                 std::shared_ptr<CallExpr>,
                 std::shared_ptr<ConditionalExpr>,
                 std::shared_ptr<StringExpr>,
                 std::shared_ptr<ParenExpr>,
                 std::shared_ptr<IntegerLit>,
                 std::shared_ptr<FloatLit>,
                 std::shared_ptr<StringLit>,
                 std::shared_ptr<DurationLit>,
                 std::shared_ptr<UintLit>,
                 std::shared_ptr<BooleanLit>,
                 std::shared_ptr<DateTimeLit>,
                 std::shared_ptr<RegexpLit>,
                 std::shared_ptr<PipeLit>,
                 std::shared_ptr<LabelLit>,
                 std::shared_ptr<BadExpr>>
        expr;

    std::shared_ptr<BaseNode> base() {
        switch (type) {
        case Type::Identifier:
            return std::get<std::shared_ptr<Identifier>>(expr)->base;
        case Type::ArrayExpr:
            return std::get<std::shared_ptr<ArrayExpr>>(expr)->base;
        case Type::DictExpr:
            return std::get<std::shared_ptr<DictExpr>>(expr)->base;
        case Type::FunctionExpr:
            return std::get<std::shared_ptr<FunctionExpr>>(expr)->base;
        case Type::LogicalExpr:
            return std::get<std::shared_ptr<LogicalExpr>>(expr)->base;
        case Type::ObjectExpr:
            return std::get<std::shared_ptr<ObjectExpr>>(expr)->base;
        case Type::MemberExpr:
            return std::get<std::shared_ptr<MemberExpr>>(expr)->base;
        case Type::IndexExpr:
            return std::get<std::shared_ptr<IndexExpr>>(expr)->base;
        case Type::BinaryExpr:
            return std::get<std::shared_ptr<BinaryExpr>>(expr)->base;
        case Type::UnaryExpr:
            return std::get<std::shared_ptr<UnaryExpr>>(expr)->base;
        case Type::PipeExpr:
            return std::get<std::shared_ptr<PipeExpr>>(expr)->base;
        case Type::CallExpr:
            return std::get<std::shared_ptr<CallExpr>>(expr)->base;
        case Type::ConditionalExpr:
            return std::get<std::shared_ptr<ConditionalExpr>>(expr)->base;
        case Type::IntegerLit:
            return std::get<std::shared_ptr<IntegerLit>>(expr)->base;
        case Type::FloatLit:
            return std::get<std::shared_ptr<FloatLit>>(expr)->base;
        case Type::StringLit:
            return std::get<std::shared_ptr<StringLit>>(expr)->base;
        case Type::DurationLit:
            return std::get<std::shared_ptr<DurationLit>>(expr)->base;
        case Type::UnsignedIntegerLit:
            return std::get<std::shared_ptr<UintLit>>(expr)->base;
        case Type::BooleanLit:
            return std::get<std::shared_ptr<BooleanLit>>(expr)->base;
        case Type::DateTimeLit:
            return std::get<std::shared_ptr<DateTimeLit>>(expr)->base;
        case Type::RegexpLit:
            return std::get<std::shared_ptr<RegexpLit>>(expr)->base;
        case Type::PipeLit:
            return std::get<std::shared_ptr<PipeLit>>(expr)->base;
        case Type::LabelLit:
            return std::get<std::shared_ptr<LabelLit>>(expr)->base;
        case Type::BadExpr:
            return std::get<std::shared_ptr<BadExpr>>(expr)->base;
        case Type::StringExpr:
            return std::get<std::shared_ptr<StringExpr>>(expr)->base;
        case Type::ParenExpr:
            return std::get<std::shared_ptr<ParenExpr>>(expr)->base;
        }
    }
};

// operator
enum class OperatorType {
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

std::unordered_map<std::string, OperatorType> operator_map = {
    {"*", OperatorType::MultiplicationOperator},
    {"/", OperatorType::DivisionOperator},
    {"%", OperatorType::ModuloOperator},
    {"^", OperatorType::PowerOperator},
    {"+", OperatorType::AdditionOperator},
    {"-", OperatorType::SubtractionOperator},
    {"<=", OperatorType::LessThanEqualOperator},
    {"<", OperatorType::LessThanOperator},
    {">=", OperatorType::GreaterThanEqualOperator},
    {">", OperatorType::GreaterThanOperator},
    {"startswith", OperatorType::StartsWithOperator},
    {"in", OperatorType::InOperator},
    {"not", OperatorType::NotOperator},
    {"exists", OperatorType::ExistsOperator},
    {"not empty", OperatorType::NotEmptyOperator},
    {"empty", OperatorType::EmptyOperator},
    {"==", OperatorType::EqualOperator},
    {"!=", OperatorType::NotEqualOperator},
    {"=~", OperatorType::RegexpMatchOperator},
    {"!~", OperatorType::NotRegexpMatchOperator},
    {"<INVALID_OP>", OperatorType::InvalidOperator},
};

std::string operator_to_string(OperatorType op) {
    switch (op) {
    case OperatorType::MultiplicationOperator:
        return "MultiplicationOperator";
    case OperatorType::DivisionOperator:
        return "DivisionOperator";
    case OperatorType::ModuloOperator:
        return "ModuloOperator";
    case OperatorType::PowerOperator:
        return "PowerOperator";
    case OperatorType::AdditionOperator:
        return "AdditionOperator";
    case OperatorType::SubtractionOperator:
        return "SubtractionOperator";
    case OperatorType::LessThanEqualOperator:
        return "LessThanEqualOperator";
    case OperatorType::LessThanOperator:
        return "LessThanOperator";
    case OperatorType::GreaterThanEqualOperator:
        return "GreaterThanEqualOperator";
    case OperatorType::GreaterThanOperator:
        return "GreaterThanOperator";
    case OperatorType::StartsWithOperator:
        return "StartsWithOperator";
    case OperatorType::InOperator:
        return "InOperator";
    case OperatorType::NotOperator:
        return "NotOperator";
    case OperatorType::ExistsOperator:
        return "ExistsOperator";
    case OperatorType::NotEmptyOperator:
        return "NotEmptyOperator";
    case OperatorType::EmptyOperator:
        return "EmptyOperator";
    case OperatorType::EqualOperator:
        return "EqualOperator";
    case OperatorType::NotEqualOperator:
        return "NotEqualOperator";
    case OperatorType::RegexpMatchOperator:
        return "RegexpMatchOperator";
    case OperatorType::NotRegexpMatchOperator:
        return "NotRegexpMatchOperator";
    case OperatorType::InvalidOperator:
        return "InvalidOperator";
    }
}

struct Operator {};

enum class LogicalOperator {
    AndOperator,
    OrOperator,
};

// assign
struct MemberAssgn : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<MemberExpr> member;
    std::shared_ptr<Expression> init;
};

enum class AssignmentType { VariableAssignment, MemberAssignment };

struct Assignment {
    AssignmentType type;
    std::shared_ptr<VariableAssgn> variable;
    std::shared_ptr<MemberAssgn> member;
};

// property
enum class PropertyKeyType { Identifier, StringLiteral };

struct PropertyKey {
    PropertyKeyType type;
    union {
        Identifier identifier;
        StringLit stringLit;
    };
};

// function
enum class FunctionBodyType { Block, Expression };

struct Block : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<Comment>> lbrace;
    std::vector<std::shared_ptr<Statement>> body;
    std::vector<std::shared_ptr<Comment>> rbrace;
};

struct FunctionBody {
    FunctionBodyType type;
    union {
        Block block;
        Expression expression;
    };
};

// parameter

struct TvarType : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> name;
};

struct NamedType : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> name;
};

struct ArrayType : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<MonoType> element;
};

struct StreamType : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<MonoType> element;
};

struct VectorType : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<MonoType> element;
};

struct DictType : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<MonoType> key;
    std::shared_ptr<MonoType> val;
};

struct DynamicType : public BaseNode {
    std::shared_ptr<BaseNode> base;
};

struct FunctionType : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::vector<std::shared_ptr<ParameterType>> parameters;
    std::shared_ptr<MonoType> monotype;
};

struct RecordType : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> tvar;
    std::vector<std::shared_ptr<PropertyType>> properties;
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
    Type type;
    std::shared_ptr<TvarType> tvar;
    std::shared_ptr<NamedType> basic;
    std::shared_ptr<ArrayType> array;
    std::shared_ptr<StreamType> stream;
    std::shared_ptr<VectorType> vector;
    std::shared_ptr<DictType> dict;
    std::shared_ptr<DynamicType> dynamic;
    std::shared_ptr<RecordType> record;
    std::shared_ptr<FunctionType> func;
    std::shared_ptr<LabelLit> label;
};

struct Required : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> name;
    std::shared_ptr<MonoType> monotype;
};

struct Optional : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> name;
    std::shared_ptr<MonoType> monotype;
    std::shared_ptr<LabelLit> _default;
};

struct Pipe : public BaseNode {
    std::shared_ptr<BaseNode> base;
    std::shared_ptr<Identifier> name;
    std::shared_ptr<MonoType> monotype;
};

struct ParameterType {
    enum class Type {
        Required,
        Optional,
        Pipe,
    };
    Type type;
    std::shared_ptr<Required> required;
    std::shared_ptr<Optional> optional;
    std::shared_ptr<Pipe> pipe;
};

} // namespace pl
