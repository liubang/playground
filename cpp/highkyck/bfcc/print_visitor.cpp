#include "print_visitor.h"

#include <cassert>
#include <cstdio>

namespace highkyck {
namespace bfcc {

void PrintVisitor::VisitorProgram(ProgramNode* node) {
  node->Lhs()->Accept(this);
  printf("\n");
}

void PrintVisitor::VisitorBinaryNode(BinaryNode* node) {
  node->Rhs()->Accept(this);
  node->Lhs()->Accept(this);
  switch (node->Op()) {
    case BinaryOperator::Add:
      printf(" + ");
      break;
    case BinaryOperator::Sub:
      printf(" - ");
      break;
    case BinaryOperator::Mul:
      printf(" * ");
      break;
    case BinaryOperator::Div:
      printf(" / ");
      break;
    default:
      assert(0);
  }
}

void PrintVisitor::VisitorConstantNode(ConstantNode* node) {
  printf(" %d ", node->Value());
}

}  // namespace bfcc
}  // namespace highkyck
