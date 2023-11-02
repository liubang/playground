//=====================================================================
//
// ast.h -
//
// Created by liubang on 2023/11/02 15:00
// Last Modified: 2023/11/02 15:00
//
//=====================================================================
#pragma once

#include <vector>

#include "token.h"

namespace pl {

enum class ExprType {
    Identifier,
    Array,
    Dict,
    Function,
    Logical,
    Object,
    Member,
    Index,
    Binary,
    Unary,
    PipeExpr,
    Call,
    Conditional,
    StringExpr,
    Paren,
    Integer,
    Float,
    StringLit,
    Duration,
    Uint,
    Boolean,
    DateTime,
    Regexp,
    PipeLit,
    Label,
    Bad,
};

struct Expr {
    ExprType type;
};

enum class StmtType {
    Expr,
    Variable,
    Option,
    Return,
    Bad,
    TestCase,
    Builtin,
};

struct Stmt {
    StmtType type;
};

enum class AsgnType {
    Variable,
    Member,
};

struct Assignment {
    AsgnType type;
};

enum class PropKeyType {
    Identifier,
    StringLit,
};

struct PropKey {
    PropKeyType type;
};

enum class FuncBodyType {
    Block,
    Expr,
};

struct FuncBody {
    FuncBodyType type;
};

struct Attribute;
struct AttributeParam;
struct File;
struct PackageClause;
struct ImportDeclaration;
struct Identifier;
struct StringLit;

struct TypeExpr;

struct BaseNode {
    SourceLocation location;
    std::vector<Comment> comments;
    std::vector<Attribute> attributes;
    std::vector<std::string> errors;
};

struct Attribute {
    std::shared_ptr<BaseNode> base_node;
    std::string name;
    std::vector<AttributeParam> params;
};

struct AttributeParam {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<Expr> value;
    std::vector<Comment> comma;
};

struct Package {
    std::shared_ptr<BaseNode> base_node;
    std::string path;
    std::string package;
    std::vector<File> files;
};

struct File {
    std::shared_ptr<BaseNode> base_node;
    std::string name;
    std::string metadata;
    std::shared_ptr<PackageClause> pacakge;
    std::vector<ImportDeclaration> import;
    std::vector<Stmt> body;
    std::vector<Comment> eof;
};

struct PackageClause {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<Identifier> name;
};

struct ImportDeclaration {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<Identifier> alias;
    std::shared_ptr<StringLit> path;
};

struct Block {
    std::shared_ptr<BaseNode> base_node;
    std::vector<Comment> lbrace;
    std::vector<Stmt> body;
    std::vector<Comment> rbrace;
};

struct BadStmt : public Stmt {
    std::shared_ptr<BaseNode> base_node;
    std::string text;
};

struct ExprStmt : public Stmt {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<Expr> expr;
};

struct ReturnStmt : public Stmt {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<Expr> argument;
};

struct OptionStmt : public Stmt {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<Expr> argument;
};

struct BuiltinStmt : public Stmt {
    std::shared_ptr<BaseNode> base_node;
    std::vector<Comment> colon;
    std::shared_ptr<Identifier> id;
    std::shared_ptr<TypeExpr> ty;
};

enum class MonoType {
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

struct TType {
    MonoType type;
};

struct NameType : public TType {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<Identifier> identifier;
};

struct TvarType : public TType {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<Identifier> identifier;
};

struct ArrayType : public TType {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<TType> monotype;
};

struct StreamType : public TType {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<TType> monotype;
};

struct VectorType : public TType {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<TType> monotype;
};

struct DictType : public TType {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<TType> key;
    std::shared_ptr<TType> val;
};

struct DynamicType : public TType {
    std::shared_ptr<BaseNode> base_node;
};

struct ParamType;

struct FuncType : public TType {
    std::shared_ptr<BaseNode> base_node;
    std::shared_ptr<TType> monotype;
    std::vector<std::shared_ptr<ParamType>> parameters;
};

struct TypeExpr : public Expr {
    std::shared_ptr<BaseNode> base_node;
};

} // namespace pl
