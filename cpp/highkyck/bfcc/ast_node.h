#pragma once

#include <memory>

namespace highkyck {
namespace bfcc {

class AstVisitor;

class AstNode {
 public:
  virtual ~AstNode() {}
  virtual void Accept(std::shared_ptr<AstVisitor> visitor);
};

class ProgramNode : public AstNode, std::enable_shared_from_this<ProgramNode> {
 public:
  ProgramNode(std::shared_ptr<AstNode> lhs) : lhs_(lhs) {}
  void Accept(std::shared_ptr<AstVisitor> visitor) override;

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
  BinaryNode(std::shared_ptr<AstNode> lhs, std::shared_ptr<AstNode> rhs)
      : lhs_(lhs), rhs_(rhs) {}
  void Accept(std::shared_ptr<AstVisitor> visitor) override;

 private:
  BinaryOperator op_;
  std::shared_ptr<AstNode> lhs_;
  std::shared_ptr<AstNode> rhs_;
};

class ConstantNode : public AstNode,
                     public std::enable_shared_from_this<ConstantNode> {
 public:
  void Accept(std::shared_ptr<AstVisitor> visitor) override;

 private:
  int value_;
};

class AstVisitor {
 public:
  virtual void VisitorProgram(std::shared_ptr<ProgramNode> node);
  virtual void VisitorBinaryNode(std::shared_ptr<BinaryNode> node);
  virtual void VisitorConstantNode(std::shared_ptr<ConstantNode> node);
};

}  // namespace bfcc
}  // namespace highkyck
