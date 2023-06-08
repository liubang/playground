//=====================================================================
//
// tuple_iteration_basics.h -
//
// Created by liubang on 2023/05/21 23:20
// Last Modified: 2023/05/21 23:20
//
//=====================================================================
#pragma once

#include <iostream>
#include <ostream>
#include <utility>

// https://www.cppstories.com/2022/tuple-iteration-basics/
namespace playground::cpp::meta {

template <typename T>
void PrintElem(const T& t) {
  std::cout << t << ',';
}

template <typename TupleT, std::size_t... Is>
void PrintTupleManual(const TupleT& tp) {
  // fold expression
  (PrintElem(std::get<Is>(tp)), ...);
}

template <typename TupleT, std::size_t... Is>
void PrintTupleManualEx(const TupleT& tp, std::index_sequence<Is...>) {
  // fold expression
  (PrintElem(std::get<Is>(tp)), ...);
}

template <typename TupleT, std::size_t TupleSize = std::tuple_size_v<TupleT>>
void PrintTupleAutoGetSize(const TupleT& tp) {
  PrintTupleManualEx(tp, std::make_index_sequence<TupleSize>{});
}

template <typename TupleT, std::size_t... Is>
void PrintTupleImpl(const TupleT& tp, std::index_sequence<Is...>) {
  std::size_t index = 0;
  auto print_elem = [&index](const auto& x) {
    if (index++ > 0)
      std::cout << ", ";
    std::cout << x;
  };

  std::cout << "(";
  (print_elem(std::get<Is>(tp)), ...);
  std::cout << ")";
}

template <typename TupleT, std::size_t TupleSize = std::tuple_size_v<TupleT>>
void PrintTupleFinal(const TupleT& tp) {
  PrintTupleImpl(tp, std::make_index_sequence<TupleSize>{});
}

template <typename TupleT, std::size_t... Is>
std::ostream& PrintTupleImplStream(std::ostream& os,
                                   const TupleT& tp,
                                   std::index_sequence<Is...>) {
  auto print_elem = [&os](const auto& x, std::size_t index) {
    if (index > 0)
      os << ", ";
    os << index << ":" << x;
  };
  os << "(";
  (print_elem(std::get<Is>(tp), Is), ...);
  os << ")";
  return os;
}

}  // namespace playground::cpp::meta

template <typename TupleT,
          std::size_t TupleSize = std::tuple_size<TupleT>::value>
inline std::ostream& operator<<(std::ostream& os, const TupleT& tp) {
  return playground::cpp::meta::PrintTupleImplStream(
      os, tp, std::make_index_sequence<TupleSize>{});
}
