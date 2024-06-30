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

#include <type_traits>

namespace pl {

//-------------------------------------------------------------------------------------------------
template <typename T> struct is_floating_point : std::false_type {};

template <> struct is_floating_point<float> : std::true_type {};

template <> struct is_floating_point<double> : std::true_type {};

template <> struct is_floating_point<long double> : std::true_type {};

template <typename T> constexpr bool is_floating_point_v = is_floating_point<T>::value;

//-------------------------------------------------------------------------------------------------
// 主模板，输入两个类型参数
template <typename T, typename U> struct is_same : std::false_type {};

// 特例化版本，待确定一个参数
template <typename T> struct is_same<T, T> : std::true_type {};

template <typename T, typename U> constexpr bool is_same_v = is_same<T, U>::value;

//-------------------------------------------------------------------------------------------------
template <typename T> struct remove_const {
    using type = T;
};

template <typename T> struct remove_const<const T> {
    using type = T;
};

template <typename T> using remove_const_t = typename remove_const<T>::type;

//-------------------------------------------------------------------------------------------------
// 主模板，输入三个模板参数
template <bool v, typename Then, typename Else> struct conditional {
    using type = Then;
};

// 偏特化版本，待确定两个类型参数，当v为false的时候
// 优先匹配此特例化版本
template <typename Then, typename Else> struct conditional<false, Then, Else> {
    using type = Else;
};

//-------------------------------------------------------------------------------------------------
template <typename T> struct array_size;

template <typename E, std::size_t N> struct array_size<E[N]> {
    using value_type = E;
    static constexpr std::size_t value = N;
};

//-------------------------------------------------------------------------------------------------
template <bool, typename = void> struct enable_if {};

template <typename T> struct enable_if<true, T> {
    using type = T;
};

template <bool v, typename T = void> using enable_if_t = typename enable_if<v, T>::type;

//-------------------------------------------------------------------------------------------------
template <typename...> using void_t = void;

/**
 * @brief 判断一个类型中是否存在type这个成员
 *
 * @tparam T
 */
template <typename T, typename = void> struct HasTypeMember : std::false_type {};

template <typename T> struct HasTypeMember<T, void_t<typename T::type>> : std::true_type {};

/**
 * @brief 判断一个类型中是否存在Init成员函数
 *
 * @tparam T
 */
template <typename T, typename = void> struct HasInit : std::false_type {};

template <typename T>
struct HasInit<T, void_t<decltype(std::declval<T>().Init())>> : std::true_type {};

//-------------------------------------------------------------------------------------------------

template <typename... Ts> struct common_type {};

template <typename T> struct common_type<T> {
    using type = T;
};

template <typename T, typename S> struct common_type<T, S> {
    using type = decltype(false ? std::declval<T>() : std::declval<S>());
};

template <typename T0, typename T1, typename... Ts> struct common_type<T0, T1, Ts...> {
    using type = typename common_type<T0, typename common_type<T1, Ts...>::type>::type;
};

/**
 * @brief 获取几个类型的公共类型
 *
 */
template <typename... Ts> using common_type_t = typename common_type<Ts...>::type;

} // namespace pl
