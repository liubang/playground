#pragma once

#include "ast_node.h"

namespace highkyck {
namespace bfcc {

class PrintVisitor : public AstVisitor {
 public:
  void VisitorProgram(ProgramNode* node) override;
  void VisitorBinaryNode(BinaryNode* node) override;
  void VisitorConstantNode(ConstantNode* node) override;
};

}  // namespace bfcc
}  // namespace highkyck
