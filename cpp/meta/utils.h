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

#include "cpp/meta/type_list.h"

#include <array>
#include <iostream>

namespace pl {

//-------------------------------------------------------------------------------------------------

/**
 * @brief 构造多维数组
 *
 * @tparam T
 */
template <typename T, std::size_t I, std::size_t... Is> struct Array {
    using type = std::array<typename Array<T, Is...>::type, I>;
};

/**
 * @brief 边界处理
 *
 * @tparam T
 */
template <typename T, std::size_t I> struct Array<T, I> {
    using type = std::array<T, I>;
};

//-------------------------------------------------------------------------------------------------

/**
 * @brief 打印
 *
 * @tparam Args
 * @param args
 */
template <typename... Args> void print(const Args&... args) {
    ((std::cout << args << std::endl), ...);
}

//-------------------------------------------------------------------------------------------------

/**
 * @brief 元函数声明，接收一个TypeList和一个单参元函数
 */
template <TL In, template <typename> class F> struct Map;

/**
 * @brief 元函数的实现
 *
 * @tparam Ts
 */
template <template <typename> class F, typename... Ts>
struct Map<TypeList<Ts...>, F> : TypeList<typename F<Ts>::type...> {};

//-------------------------------------------------------------------------------------------------

template <TL In, template <typename> class P, TL Out = TypeList<>>
struct Filter : Out {}; // 边界情况，当列表为空的时候返回空列表

template <template <typename> class P, TL Out, typename H, typename... Ts>
struct Filter<TypeList<H, Ts...>, P, Out>
    : std::conditional_t<P<H>::value,
                         Filter<TypeList<Ts...>, P, typename Out::template append<H>>,
                         Filter<TypeList<Ts...>, P, Out>> {};

//-------------------------------------------------------------------------------------------------

template <typename T> struct Return {
    using type = T;
};

template <TL In, typename Init, template <typename, typename> class Op>
struct Fold : Return<Init> {};

template <typename Acc, template <typename, typename> class Op, typename H, typename... Ts>
struct Fold<TypeList<H, Ts...>, Acc, Op> : Fold<TypeList<Ts...>, typename Op<Acc, H>::type, Op> {};

//-------------------------------------------------------------------------------------------------

template <TL... In> struct Concat;

template <TL... In> using Concat_t = typename Concat<In...>::type;

template <> struct Concat<> : TypeList<> {}; // 没有输入，返回空

template <TL In> struct Concat<In> : In {}; // 只有一个输入，返回输入本身

// 多于两个元素，递归执行concat
template <TL In, TL In2, TL... Rest>
struct Concat<In, In2, Rest...> : Concat_t<Concat_t<In, In2>, Rest...> {};

// template <TL In1, TL In2>
// struct Concat<In1, In2> {
//   template <TL Acc, typename E>
//   using Append = typename Acc::template append<E>;
//
//   using type = Fold<In2, In1, Append>::type;
// };

// template <TL In1, TL In2>
// struct Concat<In1, In2> : In2::template to<In1::template append> {};

template <typename... Ts1, typename... Ts2>
struct Concat<TypeList<Ts1...>, TypeList<Ts2...>> : TypeList<Ts1..., Ts2...> {};

//-------------------------------------------------------------------------------------------------

template <TL In, typename E> class Elem {
    template <typename Acc, typename T>
    using FindE = std::conditional_t<Acc::value, Acc, std::is_same<T, E>>;
    using Found = typename Fold<In, std::false_type, FindE>::type;

public:
    constexpr static bool value = Found::value;
};

//-------------------------------------------------------------------------------------------------

template <TL In> class Unique {
    template <TL Acc, typename E>
    // Acc为去重列表, 初始值为空, 如果E在Acc中，则返回Acc，否则将E加入Acc
    using Append = std::conditional_t<Elem<Acc, E>::value, Acc, typename Acc::template append<E>>;

public:
    using type = typename Fold<In, TypeList<>, Append>::type;
};

//-------------------------------------------------------------------------------------------------

template <typename... Ts, std::enable_if_t<std::conjunction_v<std::is_integral<Ts>...>>* = nullptr>
constexpr auto sums(Ts... ts) {
    // static_assert(std::conjunction<std::is_integral<Ts>...>::value);
    return (0 + ... + ts);
}

} // namespace pl
