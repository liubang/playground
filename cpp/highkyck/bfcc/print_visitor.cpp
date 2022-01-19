#include "print_visitor.h"

#include <cassert>
#include <cstdio>

namespace highkyck {
namespace bfcc {

void PrintVisitor::VisitorProgram(std::shared_ptr<ProgramNode> node) {
  node->Lhs()->Accept(shared_from_this());
  printf("\n");
}

void PrintVisitor::VisitorBinaryNode(std::shared_ptr<BinaryNode> node) {
  node->Rhs()->Accept(shared_from_this());
  node->Lhs()->Accept(shared_from_this());
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

void PrintVisitor::VisitorConstantNode(std::shared_ptr<ConstantNode> node) {
  printf(" %d ", node->Value());
}

}  // namespace bfcc
}  // namespace highkyck
