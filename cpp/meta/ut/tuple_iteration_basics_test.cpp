//=====================================================================
//
// tuple_iteration_basics_test.cpp -
//
// Created by liubang on 2023/05/21 23:23
// Last Modified: 2023/05/21 23:23
//
//=====================================================================

#include "cpp/meta/tuple_iteration_basics.h"

#include <catch2/catch_test_macros.hpp>
#include <tuple>
#include <utility>

TEST_CASE("meta", "[PrintTupleManual]") {
  std::tuple tp{10, 20, "hello"};
  pl::PrintTupleManual<decltype(tp), 0, 1, 2>(tp);
  std::cout << std::endl;
};

TEST_CASE("meta", "[PrintTupleManualEx]") {
  std::tuple tp{10, 20, "hello"};
  pl::PrintTupleManualEx(tp, std::index_sequence<0, 1, 2>{});
  std::cout << std::endl;
  pl::PrintTupleManualEx(
      tp, std::make_index_sequence<std::tuple_size_v<decltype(tp)>>{});
  std::cout << std::endl;
};

TEST_CASE("meta", "[PrintTupleAutoGetSize]") {
  std::tuple tp{10, 20, "hello"};
  pl::PrintTupleAutoGetSize(tp);
  std::cout << std::endl;
};

TEST_CASE("meta", "[PrintTupleFinal]") {
  std::tuple tp{10, 20, "hello"};
  pl::PrintTupleFinal(tp);
  std::cout << std::endl;
};

TEST_CASE("meta", "[PrintTupleWithOstream]") {
  std::tuple tp{10, 20, "hello"};
  std::cout << tp << std::endl;
};
