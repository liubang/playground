#pragma once

#include <memory>

namespace highkyck {
namespace bfcc {

class AstVisitor;

class AstNode {
 public:
  virtual ~AstNode() {}
  virtual void Accept(std::shared_ptr<AstVisitor> visitor) = 0;
};

class ProgramNode : public AstNode, std::enable_shared_from_this<ProgramNode> {
 public:
  ProgramNode(std::shared_ptr<AstNode> lhs) : lhs_(lhs) {}
  void Accept(std::shared_ptr<AstVisitor> visitor) override;
  std::shared_ptr<AstNode> Lhs() const { return lhs_; }

 private:
  std::shared_ptr<AstNode> lhs_;
};

enum class BinaryOperator {
  Add,
  Sub,
  Mul,
  Div,
};

class BinaryNode : public AstNode,
                   public std::enable_shared_from_this<BinaryNode> {
 public:
  BinaryNode(BinaryOperator op, std::shared_ptr<AstNode> lhs,
             std::shared_ptr<AstNode> rhs)
      : op_(op), lhs_(lhs), rhs_(rhs) {}
  void Accept(std::shared_ptr<AstVisitor> visitor) override;
  std::shared_ptr<AstNode> Lhs() const { return lhs_; }
  std::shared_ptr<AstNode> Rhs() const { return rhs_; }
  BinaryOperator Op() const { return op_; }

 private:
  BinaryOperator op_;
  std::shared_ptr<AstNode> lhs_;
  std::shared_ptr<AstNode> rhs_;
};

class ConstantNode : public AstNode,
                     public std::enable_shared_from_this<ConstantNode> {
 public:
  ConstantNode(int value) : value_(value) {}
  void Accept(std::shared_ptr<AstVisitor> visitor) override;
  int Value() const { return value_; }

 private:
  int value_;
};

class AstVisitor {
 public:
  virtual void VisitorProgram(std::shared_ptr<ProgramNode> node) = 0;
  virtual void VisitorBinaryNode(std::shared_ptr<BinaryNode> node) = 0;
  virtual void VisitorConstantNode(std::shared_ptr<ConstantNode> node) = 0;
};

}  // namespace bfcc
}  // namespace highkyck
