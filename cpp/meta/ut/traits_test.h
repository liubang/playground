// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#pragma once

#include <cmath>
#include <limits>
#include <type_traits>

#include "cpp/meta/traits.h"

namespace pl::ut {

template <typename T, enable_if_t<std::is_integral_v<T>>* = nullptr>
constexpr bool numEq(T lhs, T rhs) {
    return lhs == rhs;
}

template <typename T, enable_if_t<is_floating_point_v<T>>* = nullptr> bool numEq(T lhs, T rhs) {
    return ::fabs(lhs - rhs) < std::numeric_limits<T>::epsilon();
}

template <typename T> bool numEqImpl(T lhs, T rhs, std::true_type) {
    return ::fabs(lhs - rhs) < std::numeric_limits<T>::epsilon();
}

template <typename T> bool numEqImpl(T lhs, T rhs, std::false_type) { return lhs == rhs; }

template <typename T> auto numEqNew(T lhs, T rhs) -> enable_if_t<std::is_arithmetic_v<T>, bool> {
    return numEqImpl(lhs, rhs, is_floating_point<T>{});
};

} // namespace pl::ut
