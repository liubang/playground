#include "print_visitor.h"

#include <cassert>
#include <iostream>

namespace highkyck::bfcc {

void PrintVisitor::VisitorProgram(ProgramNode* node) {
  for (auto& s : node->Funcs()) {
    s->Accept(this);
  }
  sstream_ << "\n";
}

void PrintVisitor::VisitorFunctionNode(FunctionNode* node) {
  sstream_ << node->Name() << "(";
  const auto& params = node->Params();
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i > 0) sstream_ << ", ";
    sstream_ << params[i]->name;
  }
  sstream_ << ") {";
  for (const auto& stmt : node->Stmts()) {
    stmt->Accept(this);
  }
  sstream_ << "}";
}

void PrintVisitor::VisitorIfStmtNode(IfStmtNode* node) {
  sstream_ << "if (";
  node->Cond()->Accept(this);
  sstream_ << ") ";
  node->Then()->Accept(this);
  if (node->Else() != nullptr) {
    sstream_ << " else ";
    node->Else()->Accept(this);
  }
}

void PrintVisitor::VisitorWhileStmtNode(WhileStmtNode* node) {
  sstream_ << "while (";
  node->Cond()->Accept(this);
  sstream_ << ") ";
  node->Then()->Accept(this);
}

void PrintVisitor::VisitorDoWhileStmtNode(DoWhileStmtNode* node) {
  sstream_ << "do ";
  node->Stmt()->Accept(this);
  sstream_ << " while (";
  node->Cond()->Accept(this);
  sstream_ << ")";
}

void PrintVisitor::VisitorForStmtNode(ForStmtNode* node) {
  sstream_ << "for (";
  if (node->Init() != nullptr) {
    node->Init()->Accept(this);
  }
  sstream_ << ";";
  if (node->Cond() != nullptr) {
    node->Cond()->Accept(this);
  }
  sstream_ << ";";
  if (node->Inc() != nullptr) {
    node->Inc()->Accept(this);
  }
  sstream_ << ") ";
  node->Stmt()->Accept(this);
}

void PrintVisitor::VisitorBlockStmtNode(BlockStmtNode* node) {
  sstream_ << "{";
  for (auto s : node->Stmts()) {
    s->Accept(this);
  }
  sstream_ << "}";
}

void PrintVisitor::VisitorExprStmtNode(ExprStmtNode* node) {
  if (node->Lhs() != nullptr) {
    node->Lhs()->Accept(this);
  }
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
  sstream_ << std::string(node->Id()->name);
}

void PrintVisitor::VisitorConstantNode(ConstantNode* node) {
  sstream_ << node->Value();
}

void PrintVisitor::Descripbe() const {
  std::cout << sstream_.str();
}

std::string PrintVisitor::String() const {
  return sstream_.str();
}

PrintVisitor::~PrintVisitor() {
  sstream_.str("");
  sstream_.clear();
}

}  // namespace highkyck::bfcc
