#include "ast_node.h"

namespace highkyck {
namespace bfcc {

void ProgramNode::Accept(AstVisitor* visitor) { 
  visitor->VisitorProgram(this); 
}

void BinaryNode::Accept(AstVisitor* visitor) {
  visitor->VisitorBinaryNode(this);
}

void ConstantNode::Accept(AstVisitor* visitor) {
  visitor->VisitorConstantNode(this);
}

}  // namespace bfcc
}  // namespace highkyck
