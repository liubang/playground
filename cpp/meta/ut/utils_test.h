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

namespace playground::cpp::meta {

template <typename T>
using sizeLess4 = std::bool_constant<(sizeof(T) < 4)>;

template <typename Acc, typename E>
using TypeSizeAcc = std::integral_constant<std::size_t, Acc::value + sizeof(E)>;

}  // namespace playground::cpp::meta
