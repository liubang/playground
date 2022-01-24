#pragma once

#include <memory>

namespace highkyck {
namespace bfcc {

class AstVisitor;

class AstNode {
 public:
  virtual ~AstNode() {}
  virtual void Accept(AstVisitor* visitor) = 0;
};

// 表达式
class ProgramNode : public AstNode {
 public:
  ProgramNode(std::shared_ptr<AstNode> lhs) : lhs_(lhs) {}
  void Accept(AstVisitor* visitor) override;
  std::shared_ptr<AstNode> Lhs() const { return lhs_; }

 private:
  std::shared_ptr<AstNode> lhs_;
};

// 二元操作
enum class BinaryOperator {
  Add,
  Sub,
  Mul,
  Div,
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

// 常量
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
  virtual void VisitorBinaryNode(BinaryNode* node) = 0;
  virtual void VisitorConstantNode(ConstantNode* node) = 0;
};

}  // namespace bfcc
}  // namespace highkyck
