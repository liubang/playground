#include "ast_node.h"

namespace highkyck {
namespace bfcc {

void ProgramNode::Accept(std::shared_ptr<AstVisitor> visitor) {
  visitor->VisitorProgram(shared_from_this());
}

void BinaryNode::Accept(std::shared_ptr<AstVisitor> visitor) {
  visitor->VisitorBinaryNode(shared_from_this());
}

void ConstantNode::Accept(std::shared_ptr<AstVisitor> visitor) {
  visitor->VisitorConstantNode(shared_from_this());
}

}  // namespace bfcc
}  // namespace highkyck
