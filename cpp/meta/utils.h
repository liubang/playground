#pragma once

#include <array>
#include <iostream>

#include "cpp/meta/type_list.h"

namespace playground::cpp::meta {

//-------------------------------------------------------------------------------------------------

/**
 * @brief 构造多维数组
 *
 * @tparam T [TODO:tparam]
 */
template <typename T, std::size_t I, std::size_t... Is>
struct Array {
  using type = std::array<typename Array<T, Is...>::type, I>;
};

/**
 * @brief 边界处理
 *
 * @tparam T [TODO:tparam]
 */
template <typename T, std::size_t I>
struct Array<T, I> {
  using type = std::array<T, I>;
};

//-------------------------------------------------------------------------------------------------

/**
 * @brief 打印
 *
 * @tparam Args [TODO:tparam]
 * @param args [TODO:parameter]
 */
template <typename... Args>
void print(const Args&... args) {
  ((std::cout << args << std::endl), ...);
}

//-------------------------------------------------------------------------------------------------

/**
 * @brief 元函数声明，接收一个TypeList和一个单参元函数
 */
template <TL In, template <typename> class F>
struct Map;

/**
 * @brief 元函数的实现
 *
 * @tparam Ts [TODO:tparam]
 */
template <template <typename> class F, typename... Ts>
struct Map<TypeList<Ts...>, F> : TypeList<typename F<Ts>::type...> {};

//-------------------------------------------------------------------------------------------------

template <TL In, template <typename> class P, TL Out = TypeList<>>
struct Filter : Out {};  // 边界情况，当列表为空的时候返回空列表

template <template <typename> class P, TL Out, typename H, typename... Ts>
struct Filter<TypeList<H, Ts...>, P, Out>
    : std::conditional_t<P<H>::value, Filter<TypeList<Ts...>, P, typename Out::template append<H>>,
                         Filter<TypeList<Ts...>, P, Out>> {};

}  // namespace playground::cpp::meta
