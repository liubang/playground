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
  void Descripbe() const;
  std::string String() const;
  virtual ~PrintVisitor() {
    sstream_.str("");
    sstream_.clear();
  }

 private:
  void VisitorExprStmtNode(ExprStmtNode* node) override;
  void VisitorAssignStmtNode(AssignExprNode* node) override;
  void VisitorBinaryNode(BinaryNode* node) override;
  void VisitorIdentifierNode(IdentifierNode* node) override;
  void VisitorConstantNode(ConstantNode* node) override;

 private:
  std::stringstream sstream_;
};

}  // namespace bfcc
}  // namespace highkyck
