#pragma once

#include "ast_node.h"

namespace highkyck {
namespace bfcc {

class CodeGen : public AstVisitor {
 public:
  CodeGen() {}

  void VisitorProgram(ProgramNode* node) override;

 private:
  void VisitorBinaryNode(BinaryNode* node) override;
  void VisitorConstantNode(ConstantNode* node) override;

  void Push();
  void Pop(const char* reg);

 private:
  int stack_level_{0};
};

}  // namespace bfcc
}  // namespace highkyck
