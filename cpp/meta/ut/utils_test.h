//=====================================================================
//
// utils_test.h -
//
// Created by liubang on 2023/06/04 20:06
// Last Modified: 2023/06/04 20:06
//
//=====================================================================
#pragma once

#include <type_traits>

namespace pl::meta {

template <typename T>
using sizeLess4 = std::bool_constant<(sizeof(T) < 4)>;

template <typename Acc, typename E>
using TypeSizeAcc = std::integral_constant<std::size_t, Acc::value + sizeof(E)>;

}  // namespace pl::meta
