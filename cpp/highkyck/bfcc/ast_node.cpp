#include "ast_node.h"

namespace highkyck {
namespace bfcc {

void ProgramNode::Accept(AstVisitor* visitor) {
  visitor->VisitorProgram(this);
}

void ExprStmtNode::Accept(AstVisitor* visitor) {
  visitor->VisitorExprStmtNode(this);
}

void IfStmtNode::Accept(AstVisitor* visitor) {
  visitor->VisitorIfStmtNode(this);
}

void WhileStmtNode::Accept(AstVisitor* visitor) {
  visitor->VisitorWhileStmtNode(this);
}

void DoWhileStmtNode::Accept(AstVisitor* visitor) {
  visitor->VisitorDoWhileStmtNode(this);
}

void BlockStmtNode::Accept(AstVisitor* visitor) {
  visitor->VisitorBlockStmtNode(this);
}

void AssignExprNode::Accept(AstVisitor* visitor) {
  visitor->VisitorAssignStmtNode(this);
}

void BinaryNode::Accept(AstVisitor* visitor) {
  visitor->VisitorBinaryNode(this);
}

void IdentifierNode::Accept(AstVisitor* visitor) {
  visitor->VisitorIdentifierNode(this);
}

void ConstantNode::Accept(AstVisitor* visitor) {
  visitor->VisitorConstantNode(this);
}

}  // namespace bfcc
}  // namespace highkyck
