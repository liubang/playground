#include "ast_node.h"

namespace highkyck::bfcc {

void ProgramNode::Accept(AstVisitor* visitor) {
  visitor->VisitorProgram(this);
}

void FunctionNode::Accept(AstVisitor* visitor) {
  visitor->VisitorFunctionNode(this);
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

void ForStmtNode::Accept(AstVisitor* visitor) {
  visitor->VisitorForStmtNode(this);
}

void BlockStmtNode::Accept(AstVisitor* visitor) {
  visitor->VisitorBlockStmtNode(this);
}

void ReturnStmtNode::Accept(AstVisitor* visitor) {
  visitor->VisitorReturnStmtNode(this);
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

void FuncCallNode::Accept(AstVisitor* visitor) {
  visitor->VisitorFuncCallNode(this);
}

void ConstantNode::Accept(AstVisitor* visitor) {
  visitor->VisitorConstantNode(this);
}

}  // namespace highkyck::bfcc
