#pragma once

#include <memory>
#include <unordered_map>

#include "ast_node.h"
#include "lexer.h"

namespace highkyck::bfcc {

class Parser
{
public:
  explicit Parser(Lexer *lexer_ptr) : lexer_ptr_(lexer_ptr) {}
  std::shared_ptr<ProgramNode> Parse();

private:
  std::shared_ptr<AstNode> ParseFunction();
  std::shared_ptr<AstNode> ParseStmt();
  std::shared_ptr<AstNode> ParseExpr();
  std::shared_ptr<AstNode> ParseAssignExpr();
  std::shared_ptr<AstNode> ParseEqualExpr();
  std::shared_ptr<AstNode> ParseRelationalExpr();
  std::shared_ptr<AstNode> ParseAddExpr();
  std::shared_ptr<AstNode> ParseMultiExpr();
  std::shared_ptr<AstNode> ParsePrimaryExpr();
  std::shared_ptr<AstNode> ParseFuncCallNode();

  // sth. about variables
  std::shared_ptr<Identifier> FindId(std::string_view name);
  std::shared_ptr<Identifier> MakeId(std::string_view name);

private:
  Lexer *lexer_ptr_;
  std::list<std::shared_ptr<Identifier>> *ids_{ nullptr };
  std::unordered_map<std::string_view, std::shared_ptr<Identifier>> ids_map_;// for search
};

}// namespace highkyck::bfcc
