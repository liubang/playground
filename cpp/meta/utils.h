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
    : std::conditional_t<
          P<H>::value,
          Filter<TypeList<Ts...>, P, typename Out::template append<H>>,
          Filter<TypeList<Ts...>, P, Out>> {};

//-------------------------------------------------------------------------------------------------

template <typename T>
struct Return {
  using type = T;
};

template <TL In, typename Init, template <typename, typename> class Op>
struct Fold : Return<Init> {};

template <typename Acc,
          template <typename, typename>
          class Op,
          typename H,
          typename... Ts>
struct Fold<TypeList<H, Ts...>, Acc, Op>
    : Fold<TypeList<Ts...>, typename Op<Acc, H>::type, Op> {};

//-------------------------------------------------------------------------------------------------

template <TL... In>
struct Concat;

template <TL... In>
using Concat_t = typename Concat<In...>::type;

template <>
struct Concat<> : TypeList<> {};  // 没有输入，返回空

template <TL In>
struct Concat<In> : In {};  // 只有一个输入，返回输入本身

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

template <TL In, typename E>
class Elem {
  template <typename Acc, typename T>
  using FindE = std::conditional_t<Acc::value, Acc, std::is_same<T, E>>;
  using Found = Fold<In, std::false_type, FindE>::type;

 public:
  constexpr static bool value = Found::value;
};

//-------------------------------------------------------------------------------------------------

template <TL In>
class Unique {
  template <TL Acc, typename E>
  // Acc为去重列表, 初始值为空, 如果E在Acc中，则返回Acc，否则将E加入Acc
  using Append = std::
      conditional_t<Elem<Acc, E>::value, Acc, typename Acc::template append<E>>;

 public:
  using type = Fold<In, TypeList<>, Append>::type;
};

//-------------------------------------------------------------------------------------------------

}  // namespace playground::cpp::meta
