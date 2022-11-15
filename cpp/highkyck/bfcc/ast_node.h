#pragma once

#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace highkyck::bfcc {

class AstVisitor;

struct Identifier {
  std::string_view name;
  int64_t offset;
  Identifier(std::string_view name, int64_t offset)
      : name(name), offset(offset) {}
};

class AstNode {
 public:
  virtual ~AstNode() = default;
  virtual void Accept(AstVisitor* visitor) = 0;
};

// program
class ProgramNode : public AstNode {
 public:
  ProgramNode() = default;

  ProgramNode(std::list<std::shared_ptr<AstNode>> funcs)
      : funcs_(std::move(funcs)) {}

  ~ProgramNode() override = default;
  void Accept(AstVisitor* visitor) override;
  void PushFunc(const std::shared_ptr<AstNode>& func) {
    funcs_.push_back(func);
  }
  [[nodiscard]] const std::list<std::shared_ptr<AstNode>>& Funcs() const {
    return funcs_;
  }

 private:
  std::list<std::shared_ptr<AstNode>> funcs_;
};

class FunctionNode : public AstNode {
 public:
  explicit FunctionNode(std::string_view name) : name_(name) {}
  ~FunctionNode() override = default;
  void Accept(AstVisitor* visitor) override;
  void AddParams(const std::shared_ptr<Identifier>& param) {
    params_.push_back(param);
  }
  void PushLocalId(const std::shared_ptr<Identifier>& id) {
    local_ids_.push_back(id);
  }
  void PushStmt(const std::shared_ptr<AstNode>& stmt) {
    stmts_.push_back(stmt);
  }
  [[nodiscard]] std::string_view Name() const { return name_; }
  [[nodiscard]] const std::vector<std::shared_ptr<Identifier>>& Params() const {
    return params_;
  }
  [[nodiscard]] const std::list<std::shared_ptr<Identifier>>& LocalIds() const {
    return local_ids_;
  }
  [[nodiscard]] const std::list<std::shared_ptr<AstNode>>& Stmts() const {
    return stmts_;
  }

 private:
  friend class Parser;
  std::string_view name_;
  std::vector<std::shared_ptr<Identifier>> params_;
  std::list<std::shared_ptr<Identifier>> local_ids_;
  std::list<std::shared_ptr<AstNode>> stmts_;
};

// stmt
class ExprStmtNode : public AstNode {
 public:
  ExprStmtNode(std::shared_ptr<AstNode> lhs = nullptr) : lhs_(std::move(lhs)) {}
  ~ExprStmtNode() override = default;
  void Accept(AstVisitor* visitor) override;
  void SetLhs(std::shared_ptr<AstNode> lhs) { lhs_ = std::move(lhs); }
  [[nodiscard]] std::shared_ptr<AstNode> Lhs() const { return lhs_; }

 private:
  std::shared_ptr<AstNode> lhs_;
};

class IfStmtNode : public AstNode {
 public:
  IfStmtNode(std::shared_ptr<AstNode> c,
             std::shared_ptr<AstNode> t,
             std::shared_ptr<AstNode> e)
      : cond_(std::move(c)), then_(std::move(t)), else_(std::move(e)) {}
  ~IfStmtNode() override = default;
  void Accept(AstVisitor* visitor) override;
  [[nodiscard]] std::shared_ptr<AstNode> Cond() const { return cond_; }
  [[nodiscard]] std::shared_ptr<AstNode> Then() const { return then_; }
  [[nodiscard]] std::shared_ptr<AstNode> Else() const { return else_; }

 private:
  std::shared_ptr<AstNode> cond_;
  std::shared_ptr<AstNode> then_;
  std::shared_ptr<AstNode> else_;
};

class WhileStmtNode : public AstNode {
 public:
  WhileStmtNode(std::shared_ptr<AstNode> c, std::shared_ptr<AstNode> t)
      : cond_(std::move(c)), then_(std::move(t)) {}
  ~WhileStmtNode() override = default;
  void Accept(AstVisitor* visitor) override;
  [[nodiscard]] std::shared_ptr<AstNode> Cond() const { return cond_; }
  [[nodiscard]] std::shared_ptr<AstNode> Then() const { return then_; }

 private:
  std::shared_ptr<AstNode> cond_;
  std::shared_ptr<AstNode> then_;
};

class DoWhileStmtNode : public AstNode {
 public:
  DoWhileStmtNode(std::shared_ptr<AstNode> s, std::shared_ptr<AstNode> c)
      : stmt_(std::move(s)), cond_(std::move(c)) {}
  ~DoWhileStmtNode() override = default;
  void Accept(AstVisitor* visitor) override;
  [[nodiscard]] std::shared_ptr<AstNode> Stmt() const { return stmt_; }
  [[nodiscard]] std::shared_ptr<AstNode> Cond() const { return cond_; }

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
      : init_(std::move(init)),
        cond_(std::move(cond)),
        inc_(std::move(inc)),
        stmt_(std::move(stmt)) {}
  ~ForStmtNode() override = default;
  void Accept(AstVisitor* visitor) override;
  [[nodiscard]] std::shared_ptr<AstNode> Init() const { return init_; }
  [[nodiscard]] std::shared_ptr<AstNode> Cond() const { return cond_; }
  [[nodiscard]] std::shared_ptr<AstNode> Inc() const { return inc_; }
  [[nodiscard]] std::shared_ptr<AstNode> Stmt() const { return stmt_; }

 private:
  std::shared_ptr<AstNode> init_;
  std::shared_ptr<AstNode> cond_;
  std::shared_ptr<AstNode> inc_;
  std::shared_ptr<AstNode> stmt_;
};

class BlockStmtNode : public AstNode {
 public:
  BlockStmtNode() = default;
  ~BlockStmtNode() override = default;
  void Accept(AstVisitor* visitor) override;
  void AddStmt(const std::shared_ptr<AstNode>& stmt) { stmts_.push_back(stmt); }
  [[nodiscard]] const std::list<std::shared_ptr<AstNode>>& Stmts() const {
    return stmts_;
  }

 private:
  std::list<std::shared_ptr<AstNode>> stmts_;
};

class ReturnStmtNode : public AstNode {
 public:
  ReturnStmtNode(std::shared_ptr<AstNode> lhs) : lhs_(std::move(lhs)) {}
  ~ReturnStmtNode() override = default;
  void Accept(AstVisitor* visitor) override;
  [[nodiscard]] std::shared_ptr<AstNode> Lhs() const { return lhs_; }

 private:
  std::shared_ptr<AstNode> lhs_;
};

// assign expr
class AssignExprNode : public AstNode {
 public:
  AssignExprNode(std::shared_ptr<AstNode> lhs, std::shared_ptr<AstNode> rhs)
      : lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}

  ~AssignExprNode() override = default;
  void Accept(AstVisitor* visitor) override;
  [[nodiscard]] std::shared_ptr<AstNode> Lhs() const { return lhs_; }
  [[nodiscard]] std::shared_ptr<AstNode> Rhs() const { return rhs_; }

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
  BinaryNode(BinaryOperator op,
             std::shared_ptr<AstNode> lhs,
             std::shared_ptr<AstNode> rhs)
      : op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}
  void Accept(AstVisitor* visitor) override;
  [[nodiscard]] std::shared_ptr<AstNode> Lhs() const { return lhs_; }
  [[nodiscard]] std::shared_ptr<AstNode> Rhs() const { return rhs_; }
  [[nodiscard]] BinaryOperator Op() const { return op_; }

 private:
  BinaryOperator op_;
  std::shared_ptr<AstNode> lhs_;
  std::shared_ptr<AstNode> rhs_;
};

// identifier
class IdentifierNode : public AstNode {
 public:
  IdentifierNode(std::shared_ptr<Identifier> id) : id_(std::move(id)) {}
  ~IdentifierNode() override = default;
  void Accept(AstVisitor* visitor) override;
  [[nodiscard]] std::shared_ptr<Identifier> Id() const { return id_; }

 private:
  std::shared_ptr<Identifier> id_;
};

class FuncCallNode : public AstNode {
 public:
  FuncCallNode(std::string_view func_name,
               std::vector<std::shared_ptr<AstNode>> args)
      : func_name_(func_name), args_(std::move(args)) {}
  void Accept(AstVisitor* visitor) override;
  ~FuncCallNode() override = default;
  [[nodiscard]] std::string_view FuncName() const { return func_name_; }
  [[nodiscard]] const std::vector<std::shared_ptr<AstNode>>& Args() const {
    return args_;
  }

 private:
  std::string_view func_name_;
  std::vector<std::shared_ptr<AstNode>> args_;
};

// constant
class ConstantNode : public AstNode {
 public:
  ConstantNode(int value) : value_(value) {}
  void Accept(AstVisitor* visitor) override;
  [[nodiscard]] int Value() const { return value_; }

 private:
  int value_;
};

class AstVisitor {
 public:
  virtual ~AstVisitor() = default;
  virtual void VisitorProgram(ProgramNode* node) = 0;
  virtual void VisitorFunctionNode(FunctionNode* node) = 0;
  virtual void VisitorExprStmtNode(ExprStmtNode* node) = 0;
  virtual void VisitorIfStmtNode(IfStmtNode* node) = 0;
  virtual void VisitorWhileStmtNode(WhileStmtNode* node) = 0;
  virtual void VisitorDoWhileStmtNode(DoWhileStmtNode* node) = 0;
  virtual void VisitorForStmtNode(ForStmtNode* node) = 0;
  virtual void VisitorBlockStmtNode(BlockStmtNode* node) = 0;
  virtual void VisitorReturnStmtNode(ReturnStmtNode* node) = 0;
  virtual void VisitorAssignStmtNode(AssignExprNode* node) = 0;
  virtual void VisitorBinaryNode(BinaryNode* node) = 0;
  virtual void VisitorIdentifierNode(IdentifierNode* node) = 0;
  virtual void VisitorFuncCallNode(FuncCallNode* node) = 0;
  virtual void VisitorConstantNode(ConstantNode* node) = 0;
};

}  // namespace highkyck::bfcc
