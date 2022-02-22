#pragma once

#include <list>
#include <memory>
#include <string>

namespace highkyck {
namespace bfcc {

class AstVisitor;

struct Identifier {
  std::string_view name;
  int64_t offset;
  Identifier(std::string_view name, int64_t offset)
      : name(name), offset(offset) {}
};

class AstNode {
 public:
  virtual ~AstNode() {}
  virtual void Accept(AstVisitor* visitor) = 0;
};

// program
class ProgramNode : public AstNode {
 public:
  ProgramNode() = default;

  ProgramNode(const std::list<std::shared_ptr<AstNode>>& stmts)
      : stmts_(stmts) {}

  virtual ~ProgramNode() = default;
  void Accept(AstVisitor* visitor) override;
  void PushStmt(std::shared_ptr<AstNode> stmt) { stmts_.push_back(stmt); }
  const std::list<std::shared_ptr<AstNode>>& Stmts() const { return stmts_; }
  const std::list<std::shared_ptr<Identifier>>& LocalIds() const {
    return local_ids_;
  }

 private:
  friend class Parser;
  std::list<std::shared_ptr<AstNode>> stmts_;
  std::list<std::shared_ptr<Identifier>> local_ids_;
};

// stmt
class ExprStmtNode : public AstNode {
 public:
  ExprStmtNode(std::shared_ptr<AstNode> lhs = nullptr) : lhs_(lhs) {}
  virtual ~ExprStmtNode() = default;
  void Accept(AstVisitor* visitor) override;
  void SetLhs(std::shared_ptr<AstNode> lhs) { lhs_ = lhs; }
  std::shared_ptr<AstNode> Lhs() const { return lhs_; }

 private:
  std::shared_ptr<AstNode> lhs_;
};

class IfStmtNode : public AstNode {
 public:
  IfStmtNode(std::shared_ptr<AstNode> c, std::shared_ptr<AstNode> t,
             std::shared_ptr<AstNode> e)
      : cond_(c), then_(t), else_(e) {}
  virtual ~IfStmtNode() = default;
  void Accept(AstVisitor* visitor) override;
  std::shared_ptr<AstNode> Cond() const { return cond_; }
  std::shared_ptr<AstNode> Then() const { return then_; }
  std::shared_ptr<AstNode> Else() const { return else_; }

 private:
  std::shared_ptr<AstNode> cond_;
  std::shared_ptr<AstNode> then_;
  std::shared_ptr<AstNode> else_;
};

class WhileStmtNode : public AstNode {
 public:
  WhileStmtNode(std::shared_ptr<AstNode> c, std::shared_ptr<AstNode> t)
      : cond_(c), then_(t) {}
  virtual ~WhileStmtNode() = default;
  void Accept(AstVisitor* visitor) override;
  std::shared_ptr<AstNode> Cond() const { return cond_; }
  std::shared_ptr<AstNode> Then() const { return then_; }

 private:
  std::shared_ptr<AstNode> cond_;
  std::shared_ptr<AstNode> then_;
};

class DoWhileStmtNode : public AstNode {
 public:
  DoWhileStmtNode(std::shared_ptr<AstNode> s, std::shared_ptr<AstNode> c)
      : stmt_(s), cond_(c) {}
  virtual ~DoWhileStmtNode() = default;
  void Accept(AstVisitor* visitor) override;
  std::shared_ptr<AstNode> Stmt() const { return stmt_; }
  std::shared_ptr<AstNode> Cond() const { return cond_; }

 private:
  std::shared_ptr<AstNode> stmt_;
  std::shared_ptr<AstNode> cond_;
};

class ForStmtNode : public AstNode {
 public:
  ForStmtNode(std::shared_ptr<AstNode> init = nullptr,
              std::shared_ptr<AstNode> cond = nullptr,
              std::shared_ptr<AstNode> inc = nullptr,
              std::shared_ptr<AstNode> stmt = nullptr)
      : init_(init), cond_(cond), inc_(inc), stmt_(stmt) {}
  virtual ~ForStmtNode() = default;
  void Accept(AstVisitor* visitor) override;
  std::shared_ptr<AstNode> Init() const { return init_; }
  std::shared_ptr<AstNode> Cond() const { return cond_; }
  std::shared_ptr<AstNode> Inc() const { return inc_; }
  std::shared_ptr<AstNode> Stmt() const { return stmt_; }

 private:
  std::shared_ptr<AstNode> init_;
  std::shared_ptr<AstNode> cond_;
  std::shared_ptr<AstNode> inc_;
  std::shared_ptr<AstNode> stmt_;
};

class BlockStmtNode : public AstNode {
 public:
  BlockStmtNode() = default;
  virtual ~BlockStmtNode() = default;
  void Accept(AstVisitor* visitor) override;
  void AddStmt(std::shared_ptr<AstNode> stmt) { stmts_.push_back(stmt); }
  const std::list<std::shared_ptr<AstNode>>& Stmts() const { return stmts_; }

 private:
  std::list<std::shared_ptr<AstNode>> stmts_;
};

// assign expr
class AssignExprNode : public AstNode {
 public:
  AssignExprNode(std::shared_ptr<AstNode> lhs, std::shared_ptr<AstNode> rhs)
      : lhs_(lhs), rhs_(rhs) {}

  virtual ~AssignExprNode() = default;
  void Accept(AstVisitor* visitor) override;
  std::shared_ptr<AstNode> Lhs() const { return lhs_; }
  std::shared_ptr<AstNode> Rhs() const { return rhs_; }

 private:
  std::shared_ptr<AstNode> lhs_;
  std::shared_ptr<AstNode> rhs_;
};

// binary op
enum class BinaryOperator {
  Add,
  Sub,
  Mul,
  Div,
  Equal,
  PipeEqual,
  Greater,
  GreaterEqual,
  Lesser,
  LesserEqual,
};

class BinaryNode : public AstNode {
 public:
  BinaryNode(BinaryOperator op, std::shared_ptr<AstNode> lhs,
             std::shared_ptr<AstNode> rhs)
      : op_(op), lhs_(lhs), rhs_(rhs) {}
  void Accept(AstVisitor* visitor) override;
  std::shared_ptr<AstNode> Lhs() const { return lhs_; }
  std::shared_ptr<AstNode> Rhs() const { return rhs_; }
  BinaryOperator Op() const { return op_; }

 private:
  BinaryOperator op_;
  std::shared_ptr<AstNode> lhs_;
  std::shared_ptr<AstNode> rhs_;
};

// identifier
class IdentifierNode : public AstNode {
 public:
  IdentifierNode(std::shared_ptr<Identifier> id) : id_(id) {}
  virtual ~IdentifierNode() = default;
  void Accept(AstVisitor* visitor) override;
  std::shared_ptr<Identifier> Id() const { return id_; }

 private:
  std::shared_ptr<Identifier> id_;
};

// constant
class ConstantNode : public AstNode {
 public:
  ConstantNode(int value) : value_(value) {}
  void Accept(AstVisitor* visitor) override;
  int Value() const { return value_; }

 private:
  int value_;
};

class AstVisitor {
 public:
  virtual ~AstVisitor() {}
  virtual void VisitorProgram(ProgramNode* node) = 0;
  virtual void VisitorExprStmtNode(ExprStmtNode* node) = 0;
  virtual void VisitorIfStmtNode(IfStmtNode* node) = 0;
  virtual void VisitorWhileStmtNode(WhileStmtNode* node) = 0;
  virtual void VisitorDoWhileStmtNode(DoWhileStmtNode* node) = 0;
  virtual void VisitorForStmtNode(ForStmtNode* node) = 0;
  virtual void VisitorBlockStmtNode(BlockStmtNode* node) = 0;
  virtual void VisitorAssignStmtNode(AssignExprNode* node) = 0;
  virtual void VisitorBinaryNode(BinaryNode* node) = 0;
  virtual void VisitorIdentifierNode(IdentifierNode* node) = 0;
  virtual void VisitorConstantNode(ConstantNode* node) = 0;
};

}  // namespace bfcc
}  // namespace highkyck
