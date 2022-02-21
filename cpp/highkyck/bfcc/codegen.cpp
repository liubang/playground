#include "codegen.h"

#include <cassert>
#include <cstdio>

namespace highkyck {
namespace bfcc {

void CodeGen::VisitorProgram(ProgramNode* node) {
  code_ << "\t.text\n";
#ifdef __linux__
  code_ << "\t.global prog\n";
  code_ << "prog:\n";
#else
  static_assert(__APPLE__, "Only support linux and macos system");
  code_ << "\t.global _prog\n";
  code_ << "_prog:\n";
#endif
  int64_t stack_size = 0;
  for (auto& v : node->LocalIds()) {
    stack_size += 8;
    v->offset = stack_size * -1;
  }
  code_ << "\tpush %rbp\n";
  code_ << "\tmov %rsp, %rbp\n";
  code_ << "\tsub $" << stack_size << ", %rsp\n";
  for (auto& s : node->Stmts()) {
    s->Accept(this);
    assert(stack_level_ == 0);
  }
  code_ << "\tmov %rbp, %rsp\n";
  code_ << "\tpop %rbp\n";
  code_ << "\tret\n";
}

void CodeGen::VisitorExprStmtNode(ExprStmtNode* node) {
  // skip empty stmt, such as ';;'
  if (node->Lhs() != nullptr) {
    node->Lhs()->Accept(this);
  }
}

void CodeGen::VisitorIfStmtNode(IfStmtNode* node) {
  node->Cond()->Accept(this);
  int seq = sequence_++;
  // if
  code_ << "\tcmp $0, %rax\n";
  if (node->Else() != nullptr) {
    code_ << "\tje .L.else_" << seq << "\n";
  } else {
    code_ << "\tje .L.end_" << seq << "\n";
  }

  // then
  node->Then()->Accept(this);
  code_ << "\tjmp .L.end_" << seq << "\n";

  // else
  if (node->Else() != nullptr) {
    code_ << ".L.else_" << seq << ":\n";
    node->Else()->Accept(this);
    code_ << "\tjmp .L.end_" << seq << "\n";
  }

  code_ << ".L.end_" << seq << ":\n";
}

void CodeGen::VisitorWhileStmtNode(WhileStmtNode* node) {
  int seq = sequence_++;
  code_ << ".L.begin_" << seq << ":\n";
  node->Cond()->Accept(this);
  code_ << "\tcmp $0, %rax\n";
  code_ << "\tje .L.end_" << seq << "\n";
  node->Then()->Accept(this);
  code_ << "\tjmp .L.begin_" << seq << "\n";
  code_ << ".L.end_" << seq << ":\n";
}

void CodeGen::VisitorDoWhileStmtNode(DoWhileStmtNode* node) {
  int seq = sequence_++;
  code_ << ".L.begin_" << seq << ":\n";
  node->Stmt()->Accept(this);
  node->Cond()->Accept(this);
  code_ << "\tcmp $0, %rax\n";
  code_ << "\tje .L.end_" << seq << "\n";
  code_ << "\tjmp .L.begin_" << seq << "\n";
  code_ << ".L.end_" << seq << ":\n";
}

void CodeGen::VisitorBlockStmtNode(BlockStmtNode* node) {
  for (auto s : node->Stmts()) {
    s->Accept(this);
  }
}

void CodeGen::VisitorAssignStmtNode(AssignExprNode* node) {
  auto idnode = std::dynamic_pointer_cast<IdentifierNode>(node->Lhs());
  assert(idnode != nullptr);
  code_ << "\tlea " << idnode->Id()->offset << "(%rbp), %rax\n";
  Push();
  node->Rhs()->Accept(this);
  Pop("%rdi");
  code_ << "\tmov %rax, (%rdi)\n";
}

void CodeGen::VisitorBinaryNode(BinaryNode* node) {
  node->Rhs()->Accept(this);
  Push();
  node->Lhs()->Accept(this);
  Pop("%rdi");
  switch (node->Op()) {
    case BinaryOperator::Add: code_ << "\tadd %rdi, %rax\n"; break;
    case BinaryOperator::Sub: code_ << "\tsub %rdi, %rax\n"; break;
    case BinaryOperator::Mul: code_ << "\timul %rdi, %rax\n"; break;
    case BinaryOperator::Div:
      code_ << "\tcqo\n";
      code_ << "\tidiv %rdi\n";
      break;
    case BinaryOperator::Equal:
      code_ << "\tcmp %rdi, %rax\n";
      code_ << "\tsete %al\n";
      code_ << "\tmovzb %al, %rax\n";
      break;
    case BinaryOperator::PipeEqual:
      code_ << "\tcmp %rdi, %rax\n";
      code_ << "\tsetne %al\n";
      code_ << "\tmovzb %al, %rax\n";
      break;
    case BinaryOperator::Greater:
      code_ << "\tcmp %rdi, %rax\n";
      code_ << "\tsetg %al\n";
      code_ << "\tmovzb %al, %rax\n";
      break;
    case BinaryOperator::GreaterEqual:
      code_ << "\tcmp %rdi, %rax\n";
      code_ << "\tsetge %al\n";
      code_ << "\tmovzb %al, %rax\n";
      break;
    case BinaryOperator::Lesser:
      code_ << "\tcmp %rdi, %rax\n";
      code_ << "\tsetl %al\n";
      code_ << "\tmovzb %al, %rax\n";
      break;
    case BinaryOperator::LesserEqual:
      code_ << "\tcmp %rdi, %rax\n";
      code_ << "\tsetle %al\n";
      code_ << "\tmovzb %al, %rax\n";
      break;
    default: assert(0); break;
  }
}

void CodeGen::VisitorIdentifierNode(IdentifierNode* node) {
  code_ << "\tlea " << node->Id()->offset << "(%rbp), %rax\n";
  code_ << "\tmov (%rax), %rax\n";
}

void CodeGen::VisitorConstantNode(ConstantNode* node) {
  code_ << "\tmov $" << node->Value() << ", %rax\n";
}

void CodeGen::Push() {
  code_ << "\tpush %rax\n";
  stack_level_++;
}

void CodeGen::Pop(const char* reg) {
  code_ << "\tpop " << reg << "\n";
  stack_level_--;
}

}  // namespace bfcc
}  // namespace highkyck
