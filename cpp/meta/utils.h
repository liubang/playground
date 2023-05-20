#pragma once

#include <array>
#include <iostream>

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

}  // namespace playground::cpp::meta
