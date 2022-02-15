#include "parser.h"

#include <cassert>

#include "diagnostic.h"

namespace highkyck {
namespace bfcc {

std::shared_ptr<ProgramNode> Parser::Parse()
{
    auto node = std::make_shared<ProgramNode>();
    ids_      = &node->local_ids_;
    while (lexer_ptr_->CurrentToken()->Type() != TokenType::Eof) {
        node->PushStmt(ParseStmt());
    }
    return node;
}

std::shared_ptr<AstNode> Parser::ParseStmt()
{
    auto node = std::make_shared<ExprStmtNode>(ParseExpr());
    assert(lexer_ptr_->CurrentToken()->Type() == TokenType::Semicolon);
    lexer_ptr_->GetNextToken();
    return node;
}

std::shared_ptr<AstNode> Parser::ParseExpr()
{
    return ParseAssignExpr();
}

std::shared_ptr<AstNode> Parser::ParseAssignExpr()
{
    auto left = ParseEqualExpr();
    if (lexer_ptr_->CurrentToken()->Type() == TokenType::Assign) {
        lexer_ptr_->GetNextToken();
        auto node = std::make_shared<AssignExprNode>(left, ParseAssignExpr());
        return node;
    }
    return left;
}

std::shared_ptr<AstNode> Parser::ParseEqualExpr()
{
    auto left = ParseRelationalExpr();
    while (lexer_ptr_->CurrentToken()->Type() == TokenType::Equal ||
           lexer_ptr_->CurrentToken()->Type() == TokenType::PipeEqual) {
        BinaryOperator op = BinaryOperator::Equal;
        if (lexer_ptr_->CurrentToken()->Type() == TokenType::PipeEqual) {
            op = BinaryOperator::PipeEqual;
        }
        lexer_ptr_->GetNextToken();
        auto node =
            std::make_shared<BinaryNode>(op, left, ParseRelationalExpr());
        left = node;
    }
    return left;
}

std::shared_ptr<AstNode> Parser::ParseRelationalExpr()
{
    auto left = ParseAddExpr();
    while (lexer_ptr_->CurrentToken()->Type() == TokenType::Greater ||
           lexer_ptr_->CurrentToken()->Type() == TokenType::GreaterEqual ||
           lexer_ptr_->CurrentToken()->Type() == TokenType::Lesser ||
           lexer_ptr_->CurrentToken()->Type() == TokenType::LesserEqual) {
        BinaryOperator op = BinaryOperator::Greater;
        if (lexer_ptr_->CurrentToken()->Type() == TokenType::GreaterEqual) {
            op = BinaryOperator::GreaterEqual;
        } else if (lexer_ptr_->CurrentToken()->Type() == TokenType::Lesser) {
            op = BinaryOperator::Lesser;
        } else if (lexer_ptr_->CurrentToken()->Type() ==
                   TokenType::LesserEqual) {
            op = BinaryOperator::LesserEqual;
        }
        lexer_ptr_->GetNextToken();
        auto node = std::make_shared<BinaryNode>(op, left, ParseAddExpr());
        left      = node;
    }
    return left;
}

std::shared_ptr<AstNode> Parser::ParseAddExpr()
{
    std::shared_ptr<AstNode> left = ParseMultiExpr();
    while (lexer_ptr_->CurrentToken()->Type() == TokenType::Add ||
           lexer_ptr_->CurrentToken()->Type() == TokenType::Sub) {
        BinaryOperator op = lexer_ptr_->CurrentToken()->Type() == TokenType::Add
                                ? BinaryOperator::Add
                                : BinaryOperator::Sub;
        lexer_ptr_->GetNextToken();
        auto node = std::make_shared<BinaryNode>(op, left, ParseMultiExpr());
        left      = node;
    }
    return left;
}

std::shared_ptr<AstNode> Parser::ParseMultiExpr()
{
    std::shared_ptr<AstNode> left = ParsePrimaryExpr();
    while (lexer_ptr_->CurrentToken()->Type() == TokenType::Mul ||
           lexer_ptr_->CurrentToken()->Type() == TokenType::Div) {
        BinaryOperator op = lexer_ptr_->CurrentToken()->Type() == TokenType::Mul
                                ? BinaryOperator::Mul
                                : BinaryOperator::Div;
        lexer_ptr_->GetNextToken();
        auto node = std::make_shared<BinaryNode>(op, left, ParsePrimaryExpr());
        left      = node;
    }
    return left;
}

std::shared_ptr<AstNode> Parser::ParsePrimaryExpr()
{
    if (lexer_ptr_->CurrentToken()->Type() == TokenType::LParent) {
        lexer_ptr_->GetNextToken();                   // skip (
        auto node = ParseExpr();                      // parse expr
        lexer_ptr_->ExpectToken(TokenType::RParent);  // expect ) and stkip
        return node;
    } else if (lexer_ptr_->CurrentToken()->Type() == TokenType::Identifier) {
        auto name = lexer_ptr_->CurrentToken()->Content();
        auto id   = FindId(name);
        if (!id) {
            id = MakeId(name);
        }
        auto node = std::make_shared<IdentifierNode>(id);
        lexer_ptr_->GetNextToken();
        return node;
    } else if (lexer_ptr_->CurrentToken()->Type() == TokenType::Num) {
        auto node =
            std::make_shared<ConstantNode>(lexer_ptr_->CurrentToken()->Value());
        lexer_ptr_->GetNextToken();
        return node;
    } else {
        auto token = lexer_ptr_->CurrentToken();
        DiagnosticError(lexer_ptr_->SourceCode(),
                        token->Location().line,
                        token->Location().col,
                        "Not support node");
    }
}

std::shared_ptr<Identifier> Parser::FindId(std::string_view name)
{
    if (ids_map_.find(name) != ids_map_.end()) {
        return ids_map_[name];
    }
    return nullptr;
}

std::shared_ptr<Identifier> Parser::MakeId(std::string_view name)
{
    auto id = std::make_shared<Identifier>(name, 0);
    ids_->push_front(id);
    ids_map_[name] = id;
    return id;
}

}  // namespace bfcc
}  // namespace highkyck
