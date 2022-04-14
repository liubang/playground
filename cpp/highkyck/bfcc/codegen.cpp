#include "codegen.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>

namespace {
const std::array<std::string_view, 6> Reg64 = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8d", "%r9d" };
}

namespace highkyck::bfcc {

void CodeGen::VisitorProgram(ProgramNode *node)
{
  for (auto &f : node->Funcs()) { f->Accept(this); }
}

void CodeGen::VisitorFunctionNode(FunctionNode *node)
{
  cur_func_name_ = node->Name();
  code_ << ".text\n";
#ifdef __linux__
  code_ << ".global " << node->Name() << "\n";
  code_ << node->Name() << ":\n";
#else
  static_assert(__APPLE__, "Only support linux and macos system");
  code_ << "\t.global _" << node->Name() << "\n";
  code_ << "_" << node->Name() << ":\n";
#endif

  int32_t stack_size = 0;
  for (auto &v : node->LocalIds()) {
    stack_size += 8;
    v->offset = static_cast<int64_t>(stack_size * -1);
  }
  stack_size = AlignTo(stack_size, 16);

  // init function stack
  code_ << "\tpush %rbp\n";
  code_ << "\tmov %rsp, %rbp\n";
  code_ << "\tsub $" << stack_size << ", %rsp\n";

  int i = 0;
  for (auto &p : node->Params()) { code_ << "\tmov " << Reg64[i++] << ", " << p->offset << "(%rbp)\n"; }

  for (auto &s : node->Stmts()) {
    s->Accept(this);
    assert(stack_level_ == 0);
  }
  code_ << ".LReturn_" << cur_func_name_ << ":\n";
  code_ << "\tmov %rbp, %rsp\n";
  code_ << "\tpop %rbp\n";
  code_ << "\tret\n";
}

void CodeGen::VisitorExprStmtNode(ExprStmtNode *node)
{
  // skip empty stmt, such as ';;'
  if (node->Lhs() != nullptr) { node->Lhs()->Accept(this); }
}

void CodeGen::VisitorIfStmtNode(IfStmtNode *node)
{
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

void CodeGen::VisitorWhileStmtNode(WhileStmtNode *node)
{
  int seq = sequence_++;
  code_ << ".L.begin_" << seq << ":\n";
  node->Cond()->Accept(this);
  code_ << "\tcmp $0, %rax\n";
  code_ << "\tje .L.end_" << seq << "\n";
  node->Then()->Accept(this);
  code_ << "\tjmp .L.begin_" << seq << "\n";
  code_ << ".L.end_" << seq << ":\n";
}

void CodeGen::VisitorDoWhileStmtNode(DoWhileStmtNode *node)
{
  int seq = sequence_++;
  code_ << ".L.begin_" << seq << ":\n";
  node->Stmt()->Accept(this);
  node->Cond()->Accept(this);
  code_ << "\tcmp $0, %rax\n";
  code_ << "\tje .L.end_" << seq << "\n";
  code_ << "\tjmp .L.begin_" << seq << "\n";
  code_ << ".L.end_" << seq << ":\n";
}

void CodeGen::VisitorForStmtNode(ForStmtNode *node)
{
  int seq = sequence_++;
  if (node->Init()) { node->Init()->Accept(this); }
  code_ << ".L.begin_" << seq << ":\n";
  if (node->Cond()) {
    node->Cond()->Accept(this);
    code_ << "\tcmp $0, %rax\n";
    code_ << "\tje .L.end_" << seq << "\n";
  }
  node->Stmt()->Accept(this);
  if (node->Inc()) { node->Inc()->Accept(this); }
  code_ << "\tjmp .L.begin_" << seq << "\n";
  code_ << ".L.end_" << seq << ":\n";
}

void CodeGen::VisitorBlockStmtNode(BlockStmtNode *node)
{
  for (const auto &s : node->Stmts()) { s->Accept(this); }
}

void CodeGen::VisitorReturnStmtNode(ReturnStmtNode *node)
{
  node->Lhs()->Accept(this);
  code_ << "\tjmp .LReturn_" << cur_func_name_ << "\n";
}

void CodeGen::VisitorAssignStmtNode(AssignExprNode *node)
{
  auto idnode = std::dynamic_pointer_cast<IdentifierNode>(node->Lhs());
  assert(idnode != nullptr);
  code_ << "\tlea " << idnode->Id()->offset << "(%rbp), %rax\n";
  Push();
  node->Rhs()->Accept(this);
  Pop("%rdi");
  code_ << "\tmov %rax, (%rdi)\n";
}

void CodeGen::VisitorBinaryNode(BinaryNode *node)
{
  node->Rhs()->Accept(this);
  Push();
  node->Lhs()->Accept(this);
  Pop("%rdi");
  switch (node->Op()) {
  case BinaryOperator::Add:
    code_ << "\tadd %rdi, %rax\n";
    break;
  case BinaryOperator::Sub:
    code_ << "\tsub %rdi, %rax\n";
    break;
  case BinaryOperator::Mul:
    code_ << "\timul %rdi, %rax\n";
    break;
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
  default:
    assert(0);
    break;
  }
}

void CodeGen::VisitorIdentifierNode(IdentifierNode *node)
{
  code_ << "\tlea " << node->Id()->offset << "(%rbp), %rax\n";
  code_ << "\tmov (%rax), %rax\n";
}

void CodeGen::VisitorFuncCallNode(FuncCallNode *node)
{
  for (auto &arg : node->Args()) {
    arg->Accept(this);
    Push();
  }
  for (int i = node->Args().size() - 1; i >= 0; i--) { Pop(Reg64[i].data()); }
#ifdef __linux__
  code_ << "\tcall " << node->FuncName() << "\n";
#else
  static_assert(__APPLE__, "Only support linux and macos system");
  code_ << "\tcall __" << node->FuncName() << "\n";
#endif
}

void CodeGen::VisitorConstantNode(ConstantNode *node) { code_ << "\tmov $" << node->Value() << ", %rax\n"; }

void CodeGen::Push()
{
  code_ << "\tpush %rax\n";
  stack_level_++;
}

void CodeGen::Pop(const char *reg)
{
  code_ << "\tpop " << reg << "\n";
  stack_level_--;
}

int32_t CodeGen::AlignTo(int32_t size, int32_t align) { return (size + align - 1) / align * align; }

CodeGen::~CodeGen()
{
  code_.str("");
  code_.clear();
}
}// namespace highkyck::bfcc
