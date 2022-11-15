#include "type.h"

namespace highkyck::bfcc {

std::shared_ptr<BuiltInType> Type::IntType =
    std::make_shared<BuiltInType>(BuiltInType::EKind::Int, 8, 8);

bool Type::IsIntegerType() const {
  if (tc_ == TypeClass::BuiltInType) {
    auto bi = dynamic_cast<const BuiltInType*>(this);
    return bi->Kind() == BuiltInType::EKind::Int;
  }
  return false;
}

bool Type::IsFunctionType() const {
  return tc_ == TypeClass::FunctionType;
}

bool Type::IsPointerType() const {
  return tc_ == TypeClass::PointerType;
}

}  // namespace highkyck::bfcc
