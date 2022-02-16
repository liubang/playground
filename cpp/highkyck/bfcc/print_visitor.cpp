#include "print_visitor.h"

#include <cassert>
#include <iostream>

namespace highkyck {
namespace bfcc {

void PrintVisitor::VisitorProgram(ProgramNode* node) {
  for (auto& s : node->Stmts()) {
    s->Accept(this);
  }
  sstream_ << "\n";
}

void PrintVisitor::VisitorIfStmtNode(IfStmtNode* node) {
  sstream_ << "if (";
  node->Cond()->Accept(this);
  sstream_ << ")";
  node->Then()->Accept(this);
  if (node->Else() != nullptr) {
    sstream_ << " else ";
    node->Else()->Accept(this);
  }
}

void PrintVisitor::VisitorExprStmtNode(ExprStmtNode* node) {
  node->Lhs()->Accept(this);
  sstream_ << ";";
}

void PrintVisitor::VisitorAssignStmtNode(AssignExprNode* node) {
  node->Lhs()->Accept(this);
  sstream_ << " = ";
  node->Rhs()->Accept(this);
}

void PrintVisitor::VisitorBinaryNode(BinaryNode* node) {
  node->Lhs()->Accept(this);
  switch (node->Op()) {
    case BinaryOperator::Add: sstream_ << " + "; break;
    case BinaryOperator::Sub: sstream_ << " - "; break;
    case BinaryOperator::Mul: sstream_ << " * "; break;
    case BinaryOperator::Div: sstream_ << " / "; break;
    case BinaryOperator::Equal: sstream_ << " == "; break;
    case BinaryOperator::PipeEqual: sstream_ << " != "; break;
    case BinaryOperator::Greater: sstream_ << " > "; break;
    case BinaryOperator::GreaterEqual: sstream_ << " >= "; break;
    case BinaryOperator::Lesser: sstream_ << " < "; break;
    case BinaryOperator::LesserEqual: sstream_ << " <= "; break;
    default: assert(0);
  }
  node->Rhs()->Accept(this);
}

void PrintVisitor::VisitorIdentifierNode(IdentifierNode* node) {
  sstream_ << " " << std::string(node->Id()->name) << " ";
}

void PrintVisitor::VisitorConstantNode(ConstantNode* node) {
  sstream_ << " " << node->Value() << " ";
}

void PrintVisitor::Descripbe() const {
  std::cout << sstream_.str();
}

std::string PrintVisitor::String() const {
  return sstream_.str();
}

}  // namespace bfcc
}  // namespace highkyck
