#pragma once

#include <iostream>
#include <ostream>
#include <utility>

namespace highkyck {
namespace cppstories {

template <typename... Args>
void PrintImpl(const Args&... tupleArgs) {
  std::size_t index = 0;
  auto print_elem = [&index](const auto& x) {
    if (index++ > 0) std::cout << ", ";
    std::cout << x;
  };

  (print_elem(tupleArgs), ...);
}

template <typename... Args>
void PrintTupleApplyFn(const std::tuple<Args...> tp) {
  std::cout << "(";
  std::apply(PrintImpl<Args...>, tp);
  std::cout << ")";
}

template <typename TupleT>
void PrintTupleApply(const TupleT& tp) {
  std::cout << "(";
  std::apply(
      [](const auto&... tupleArgs) {
        std::size_t index = 0;
        auto print_elem = [&index](const auto& x) {
          if (index++ > 0) std::cout << ", ";
          std::cout << x;
        };
        (print_elem(tupleArgs), ...);
      },
      tp);
  std::cout << ")";
}

}  // namespace cppstories
}  // namespace highkyck
