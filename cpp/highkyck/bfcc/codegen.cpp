#include "codegen.h"

#include <cassert>
#include <cstdio>

namespace highkyck {
namespace bfcc {

void CodeGen::VisitorProgram(ProgramNode* node) {
  printf("\t.text\n");
#ifdef __linux__
  printf("\t.global prog\n");
  printf("prog:\n");
#else
  assert(__APPLE__);
  printf("\t.global _prog\n");
  printf("_prog:\n");
#endif
  printf("\tpush %%rbp\n");
  printf("\tmov %%rsp, %%rbp\n");
  printf("\tsub $32, %%rsp\n");

  node->Lhs()->Accept(this);
  assert(stack_level_ == 0);

  printf("\tmov %%rbp, %%rsp\n");
  printf("\tpop %%rbp\n");
  printf("\tret\n");
}

void CodeGen::VisitorBinaryNode(BinaryNode* node) {
  node->Rhs()->Accept(this);
  Push();
  node->Lhs()->Accept(this);
  Pop("%rdi");
  switch (node->Op()) {
    case BinaryOperator::Add:
      printf("\tadd %%rdi, %%rax\n");
      break;
    case BinaryOperator::Sub:
      printf("\tsub %%rdi, %%rax\n");
      break;
    case BinaryOperator::Mul:
      printf("\timul %%rdi, %%rax\n");
      break;
    case BinaryOperator::Div:
      printf("\tcqo\n");
      printf("\tidiv %%rdi\n");
      break;
    default:
      assert(0);
      break;
  }
}

void CodeGen::VisitorConstantNode(ConstantNode* node) {
  printf("\tmov $%d, %%rax\n", node->Value());
}

void CodeGen::Push() {
  printf("\tpush %%rax\n");
  stack_level_++;
}

void CodeGen::Pop(const char* reg) {
  printf("\tpop %s\n", reg);
  stack_level_--;
}

}  // namespace bfcc
}  // namespace highkyck
