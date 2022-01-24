#pragma once

#include <memory>

#include "ast_node.h"
#include "lexer.h"

namespace highkyck {
namespace bfcc {

class Parser {
 public:
  explicit Parser(Lexer* lexer_ptr) : lexer_ptr_(lexer_ptr) {}
  std::shared_ptr<ProgramNode> Parse();

 private:
  std::shared_ptr<AstNode> ParseExpr();
  std::shared_ptr<AstNode> ParseAddExpr();
  std::shared_ptr<AstNode> ParseMultiExpr();
  std::shared_ptr<AstNode> ParsePrimaryExpr();

 private:
  Lexer* lexer_ptr_;
};

}  // namespace bfcc
}  // namespace highkyck
