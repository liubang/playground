#include "print_visitor.h"

#include <cassert>
#include <iostream>

namespace highkyck {
namespace bfcc {

void PrintVisitor::VisitorProgram(ProgramNode* node) {
  node->Lhs()->Accept(this);
  sstream_ << "\n";
}

void PrintVisitor::VisitorBinaryNode(BinaryNode* node) {
  node->Rhs()->Accept(this);
  node->Lhs()->Accept(this);
  switch (node->Op()) {
    case BinaryOperator::Add:
      sstream_ << " + ";
      break;
    case BinaryOperator::Sub:
      sstream_ << " - ";
      break;
    case BinaryOperator::Mul:
      sstream_ << " * ";
      break;
    case BinaryOperator::Div:
      sstream_ << " / ";
      break;
    default:
      assert(0);
  }
}

void PrintVisitor::VisitorConstantNode(ConstantNode* node) {
  sstream_ << " " << node->Value() << " ";
}

void PrintVisitor::Descripbe() const { std::cout << sstream_.str(); }

std::string PrintVisitor::String() const { return sstream_.str(); }

}  // namespace bfcc
}  // namespace highkyck
