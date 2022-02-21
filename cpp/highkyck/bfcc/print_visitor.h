#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "ast_node.h"

namespace highkyck {
namespace bfcc {

class PrintVisitor : public AstVisitor {
 public:
  void VisitorProgram(ProgramNode* node) override;
  virtual ~PrintVisitor() {
    sstream_.str("");
    sstream_.clear();
  }
  void Descripbe() const;
  std::string String() const;

 private:
  void VisitorExprStmtNode(ExprStmtNode* node) override;
  void VisitorIfStmtNode(IfStmtNode* node) override;
  void VisitorWhileStmtNode(WhileStmtNode* node) override;
  void VisitorDoWhileStmtNode(DoWhileStmtNode* node) override;
  void VisitorBlockStmtNode(BlockStmtNode* node) override;
  void VisitorAssignStmtNode(AssignExprNode* node) override;
  void VisitorBinaryNode(BinaryNode* node) override;
  void VisitorIdentifierNode(IdentifierNode* node) override;
  void VisitorConstantNode(ConstantNode* node) override;

 private:
  std::stringstream sstream_;
};

}  // namespace bfcc
}  // namespace highkyck
