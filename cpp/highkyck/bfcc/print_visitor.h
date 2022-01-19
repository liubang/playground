#pragma once

#include "ast_node.h"

namespace highkyck {
namespace bfcc {

class PrintVisitor : public AstVisitor, public std::enable_shared_from_this<PrintVisitor> {
 public:
  void VisitorProgram(std::shared_ptr<ProgramNode> node) override;
  void VisitorBinaryNode(std::shared_ptr<BinaryNode> node) override;
  void VisitorConstantNode(std::shared_ptr<ConstantNode> node) override;
};

}  // namespace bfcc
}  // namespace highkyck
