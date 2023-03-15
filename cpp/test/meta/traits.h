#pragma once

#include <type_traits>

namespace highkyck::meta {

//-------------------------------------------------------------------------------------------------
template <typename T>
struct is_floating_point : std::false_type {};

template <>
struct is_floating_point<float> : std::true_type {};

template <>
struct is_floating_point<double> : std::true_type {};

template <>
struct is_floating_point<long double> : std::true_type {};

//-------------------------------------------------------------------------------------------------
// 主模板，输入两个类型参数
template <typename T, typename U>
struct is_same : std::false_type {};

// 特例化版本，待确定一个参数
template <typename T>
struct is_same<T, T> : std::true_type {};

template <typename T, typename U>
constexpr bool is_same_v = is_same<T, U>::value;

//-------------------------------------------------------------------------------------------------
template <typename T>
struct remove_const {
  using type = T;
};

template <typename T>
struct remove_const<const T> {
  using type = T;
};

template <typename T>
using remove_const_t = typename remove_const<T>::type;

//-------------------------------------------------------------------------------------------------
// 主模板，输入三个模板参数
template <bool v, typename Then, typename Else>
struct conditional {
  using type = Then;
};

// 偏特化版本，待确定两个类型参数，当v为false的时候
// 优先匹配此特例化版本
template <typename Then, typename Else>
struct conditional<false, Then, Else> {
  using type = Else;
};

//-------------------------------------------------------------------------------------------------
template <typename T>
struct array_size;

template <typename E, std::size_t N>
struct array_size<E[N]> {
  using value_type = E;
  static constexpr std::size_t value = N;
};

}  // namespace highkyck::meta
