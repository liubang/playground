#include "parser.h"

namespace highkyck {
namespace bfcc {

std::shared_ptr<AstNode> Parser::Parse() { return ParseExpr(); }

std::shared_ptr<AstNode> Parser::ParseExpr() { return ParseAddExpr(); }

std::shared_ptr<AstNode> Parser::ParseAddExpr() {
  std::shared_ptr<AstNode> left = ParseMultiExpr();
  while (lexer_ptr_->CurrentToken()->Type() == TokenType::Add ||
         lexer_ptr_->CurrentToken()->Type() == TokenType::Sub) {
    BinaryOperator op = BinaryOperator::Add;
    lexer_ptr_->GetNextToken();
  }
}

std::shared_ptr<AstNode> Parser::ParseMultiExpr() {}

std::shared_ptr<AstNode> Parser::ParsePrimaryExpr() {}

}  // namespace bfcc
}  // namespace highkyck
