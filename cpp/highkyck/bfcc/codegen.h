#pragma once

#include <sstream>

#include "ast_node.h"

namespace highkyck {
namespace bfcc {

class CodeGen : public AstVisitor {
 public:
  CodeGen() {}
  virtual ~CodeGen() {
    code_.str("");
    code_.clear();
  }

  void VisitorProgram(ProgramNode* node) override;
  std::string Code() const { return code_.str(); }

 private:
  void VisitorExprStmtNode(ExprStmtNode* node) override;
  void VisitorAssignStmtNode(AssignExprNode* node) override;
  void VisitorBinaryNode(BinaryNode* node) override;
  void VisitorIdentifierNode(IdentifierNode* node) override;
  void VisitorConstantNode(ConstantNode* node) override;

  void Push();
  void Pop(const char* reg);

 private:
  int stack_level_{0};
  std::stringstream code_;
};

}  // namespace bfcc
}  // namespace highkyck
