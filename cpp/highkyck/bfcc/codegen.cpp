#include "codegen.h"

#include <cassert>
#include <cstdio>

namespace highkyck {
namespace bfcc {

void CodeGen::VisitorProgram(ProgramNode* node) {
  code_ << "\t.text\n";
  // printf("\t.text\n");
#ifdef __linux__
  code_ << "\t.global prog\n";
  code_ << "prog:\n";
  // printf("\t.global prog\n");
  // printf("prog:\n");
#else
  assert(__APPLE__);
  code_ << "\t.global _prog\n";
  code_ << "_prog:\n";
  // printf("\t.global _prog\n");
  // printf("_prog:\n");
#endif
  code_ << "\tpush %%rbp\n";
  code_ << "\tmov %%rsp, %%rbp\n";
  code_ << "\tsub $32, %%rsp\n";
  // printf("\tpush %%rbp\n");
  // printf("\tmov %%rsp, %%rbp\n");
  // printf("\tsub $32, %%rsp\n");

  node->Lhs()->Accept(this);
  assert(stack_level_ == 0);

  code_ << "\tmov %%rbp, %%rsp\n";
  code_ << "\tpop %%rbp\n";
  code_ << "\tret\n";
  // printf("\tmov %%rbp, %%rsp\n");
  // printf("\tpop %%rbp\n");
  // printf("\tret\n");
}

void CodeGen::VisitorBinaryNode(BinaryNode* node) {
  node->Rhs()->Accept(this);
  Push();
  node->Lhs()->Accept(this);
  Pop("%rdi");
  switch (node->Op()) {
    case BinaryOperator::Add:
      code_ << "\tadd %%rdi, %%rax\n";
      // printf("\tadd %%rdi, %%rax\n");
      break;
    case BinaryOperator::Sub:
      code_ << "\tsub %%rdi, %%rax\n";
      // printf("\tsub %%rdi, %%rax\n");
      break;
    case BinaryOperator::Mul:
      code_ << "\timul %%rdi, %%rax\n";
      // printf("\timul %%rdi, %%rax\n");
      break;
    case BinaryOperator::Div:
      code_ << "\tcqo\n";
      code_ << "\tidiv %%rdi\n";
      // printf("\tcqo\n");
      // printf("\tidiv %%rdi\n");
      break;
    default:
      assert(0);
      break;
  }
}

void CodeGen::VisitorConstantNode(ConstantNode* node) {
  // printf("\tmov $%d, %%rax\n", node->Value());
  code_ << "\tmov $" << node->Value() << ", %%rax\n";
}

void CodeGen::Push() {
  code_ << "\tpush %%rax\n";
  // printf("\tpush %%rax\n");
  stack_level_++;
}

void CodeGen::Pop(const char* reg) {
  code_ << "\tpop %s\n";
  // printf("\tpop %s\n", reg);
  stack_level_--;
}

}  // namespace bfcc
}  // namespace highkyck
