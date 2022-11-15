#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <utility>

#include "lexer.h"

namespace highkyck::bfcc {

class BuiltInType;
class FunctionType;
class PointerType;

class Type {
 public:
  enum class TypeClass {
    BuiltInType,
    PointerType,
    FunctionType,
  };

 public:
  static std::shared_ptr<BuiltInType> IntType;

 public:
  Type(TypeClass tc, uint32_t size, uint32_t align)
      : tc_(tc), size_(size), align_(align) {}
  virtual ~Type() = default;

  [[nodiscard]] TypeClass Tc() const { return tc_; }

  [[nodiscard]] uint32_t Size() const { return size_; }

  [[nodiscard]] uint32_t Align() const { return align_; }

  [[nodiscard]] bool IsIntegerType() const;

  [[nodiscard]] bool IsFunctionType() const;

  [[nodiscard]] bool IsPointerType() const;

 protected:
  TypeClass tc_;
  uint32_t size_;
  uint32_t align_;
};

class BuiltInType : public Type {
 public:
  enum class EKind {
    Int,
  };

 public:
  BuiltInType(EKind kd, uint32_t size, uint32_t align)
      : Type(TypeClass::BuiltInType, size, align), kind_(kd) {}

  ~BuiltInType() override = default;

  [[nodiscard]] EKind Kind() const { return kind_; }

 private:
  EKind kind_;
};

class PointerType : public Type {
 public:
  PointerType(std::shared_ptr<Type> base, uint32_t size, uint32_t align)
      : Type(TypeClass::PointerType, size, align), base_(std::move(base)) {}

  ~PointerType() override = default;

  [[nodiscard]] std::shared_ptr<Type> Base() const { return base_; }

 private:
  std::shared_ptr<Type> base_;
};

struct Param {
  std::shared_ptr<Type> type;
  std::shared_ptr<Token> token;
};

class FunctionType : public Type {
 public:
  FunctionType(std::shared_ptr<Type> return_type, uint32_t size, uint32_t align)
      : Type(TypeClass::FunctionType, size, align),
        return_type_(std::move(return_type)) {}

  ~FunctionType() override = default;

  void SetParams(std::list<std::shared_ptr<Param>> params) {
    params_ = std::move(params);
  }

  [[nodiscard]] const std::list<std::shared_ptr<Param>>& Params() const {
    return params_;
  }

  [[nodiscard]] std::shared_ptr<Type> ReturnType() const {
    return return_type_;
  }

 private:
  std::shared_ptr<Type> return_type_;
  std::list<std::shared_ptr<Param>> params_;
};

}  // namespace highkyck::bfcc
