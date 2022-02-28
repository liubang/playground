#pragma once

#include <sstream>

#include "ast_node.h"

namespace highkyck::bfcc {

class CodeGen : public AstVisitor {
 public:
  CodeGen() = default;
  ~CodeGen() override;
  void VisitorProgram(ProgramNode* node) override;
  std::string Code() const { return code_.str(); }

 private:
  void VisitorFunctionNode(FunctionNode* node) override;
  void VisitorExprStmtNode(ExprStmtNode* node) override;
  void VisitorIfStmtNode(IfStmtNode* node) override;
  void VisitorWhileStmtNode(WhileStmtNode* node) override;
  void VisitorDoWhileStmtNode(DoWhileStmtNode* node) override;
  void VisitorForStmtNode(ForStmtNode* node) override;
  void VisitorBlockStmtNode(BlockStmtNode* node) override;
  void VisitorAssignStmtNode(AssignExprNode* node) override;
  void VisitorBinaryNode(BinaryNode* node) override;
  void VisitorIdentifierNode(IdentifierNode* node) override;
  void VisitorFuncCallNode(FuncCallNode* node) override;
  void VisitorConstantNode(ConstantNode* node) override;

  void Push();
  void Pop(const char* reg);

  int32_t AlignTo(int32_t size, int32_t align);

 private:
  int stack_level_{0};
  std::stringstream code_;
  int sequence_{0};
};

}  // namespace highkyck::bfcc
