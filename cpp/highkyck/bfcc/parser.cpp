#include "parser.h"

namespace highkyck {
namespace bfcc {

std::shared_ptr<ProgramNode> Parser::Parse() {
  auto node = std::make_shared<ProgramNode>(ParseExpr());
  return node;
}

std::shared_ptr<AstNode> Parser::ParseExpr() { return ParseAddExpr(); }

std::shared_ptr<AstNode> Parser::ParseAddExpr() {
  std::shared_ptr<AstNode> left = ParseMultiExpr();
  while (lexer_ptr_->CurrentToken()->Type() == TokenType::Add ||
         lexer_ptr_->CurrentToken()->Type() == TokenType::Sub) {
    BinaryOperator op = lexer_ptr_->CurrentToken()->Type() == TokenType::Add
                            ? BinaryOperator::Add
                            : BinaryOperator::Sub;
    lexer_ptr_->GetNextToken();
    auto node = std::make_shared<BinaryNode>(op, left, ParseMultiExpr());
    left = node;
  }
  return left;
}

std::shared_ptr<AstNode> Parser::ParseMultiExpr() {
  std::shared_ptr<AstNode> left = ParsePrimaryExpr();
  while (lexer_ptr_->CurrentToken()->Type() == TokenType::Mul ||
         lexer_ptr_->CurrentToken()->Type() == TokenType::Div) {
    BinaryOperator op = lexer_ptr_->CurrentToken()->Type() == TokenType::Mul
                            ? BinaryOperator::Mul
                            : BinaryOperator::Div;
    lexer_ptr_->GetNextToken();
    auto node = std::make_shared<BinaryNode>(op, left, ParsePrimaryExpr());
    left = node;
  }
  return left;
}

std::shared_ptr<AstNode> Parser::ParsePrimaryExpr() {
  if (lexer_ptr_->CurrentToken()->Type() == TokenType::LParent) {
    lexer_ptr_->GetNextToken();
    auto node = ParseExpr();
    lexer_ptr_->GetNextToken();
    return node;
  } else {
    auto node =
        std::make_shared<ConstantNode>(lexer_ptr_->CurrentToken()->Value());
    lexer_ptr_->GetNextToken();
    return node;
  }
}

}  // namespace bfcc
}  // namespace highkyck
