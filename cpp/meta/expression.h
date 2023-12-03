// Copyright (c) 2023 The Authors. All rights reserved.
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

#include <cassert>

#include "traits.h"

namespace pl {

template <typename T, typename U, typename OP> struct BinaryExpression {
    BinaryExpression(const T& lhs, const U& rhs, OP op) : lhs(lhs), rhs(rhs), op(op) {}

    auto operator()() const { return op(lhs, rhs); }

protected:
    T lhs;
    U rhs;
    OP op;
};

template <typename T, typename U, typename OP>
struct BinaryContainerExpression : private BinaryExpression<T, U, OP> {
    using Base = BinaryExpression<T, U, OP>;
    using Base::Base; // 继承基类的构造函数
    //
    auto operator[](std::size_t index) const {
        assert(index < size());
        return Base::op(Base::lhs[index], Base::rhs[index]);
    }

    [[nodiscard]] std::size_t size() const {
        assert(Base::lhs.size() == Base::rhs.size());
        return Base::lhs.size();
    }
};

template <typename T, typename U, typename OP> // 类模板参数推导规则
BinaryContainerExpression(T, U, OP) -> BinaryContainerExpression<T, U, OP>;

//-------------------------------------------------------------------------------------------------
/**
 * @brief 判断一个类型是否为容器
 *
 * @tparam T
 */
template <typename T, typename = void> constexpr bool is_container_v = false;

// 这里简单的用类型是否存在value_type和iterator来判断是否为容器类型
template <typename T>
constexpr bool is_container_v<T, void_t<typename T::value_type, typename T::iterator>> = true;

// BinaryContainerExpression也视为容器类型
template <typename T, typename U, typename OP>
constexpr bool is_container_v<BinaryContainerExpression<T, U, OP>> = true;

} // namespace pl

/**
 * @brief 重载operator+，这里必须在namespace外面定义
 *
 * @tparam T
 * @tparam U
 * @param lhs
 * @param rhs
 * @return
 */
template <typename T,
          typename U,
          typename = pl::enable_if_t<pl::is_container_v<T> && pl::is_container_v<U>>>
auto operator+(const T& lhs, const U& rhs) {
    auto plus = [](auto x, auto y) {
        return x + y;
    };
    return pl::BinaryContainerExpression(lhs, rhs, plus);
}
