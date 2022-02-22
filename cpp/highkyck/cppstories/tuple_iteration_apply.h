#pragma once

#include "tuple_iteration_basics.h"

namespace highkyck {
namespace cppstories {

template <typename... Args>
void PrintTupleApplyFn(const std::tuple<Args...> tp) {
  std::cout << "(";
  std::apply(PrintTupleImpl<Args...>, tp);
  std::cout << ")";
}

}  // namespace cppstories
}  // namespace highkyck
