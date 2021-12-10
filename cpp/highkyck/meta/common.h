#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>

#define HK_CONCATENATE_IMPL(s1, s2) s1##s2
#define HK_CONCATENATE(s1, s2) HK_CONCATENATE_IMPL(s1, s2)
#define HK_ANONYMOUS_VARIABLE(name) \
  HK_CONCATENATE(HK_CONCATENATE(HK_CONCATENATE(name, __COUNTER__), _), __LINE__)

namespace highkyck {
namespace detail {

template <typename T>
std::string type_name() {
  typedef typename std::remove_reference<T>::type TR;
  std::unique_ptr<char, void (*)(void*)> own(nullptr, std::free);
  std::string r = own != nullptr ? own.get() : typeid(TR).name();
  if (std::is_const<TR>::value) {
    r += " const";
  }
  if (std::is_volatile<TR>::value) {
    r += " volatile";
  }
  if (std::is_lvalue_reference<T>::value) {
    r += "&";
  }
  if (std::is_rvalue_reference<T>::value) {
    r += "&&";
  }
  return r;
}

template <typename... Ts>
struct TypeList {
  using type = TypeList<Ts...>;
  constexpr static std::size_t size = sizeof...(Ts);

  // TypeList<int, char>::append<long, std::string>
  template <typename... T>
  using append = TypeList<Ts..., T...>;

  // TypeList<int, char>::export_to<std::tuple> => std::tuple<itn, char>;
  template <template <typename...> typename T>
  using export_to = T<Ts...>;
};

template <typename T, T v>
struct integral_constant {
  constexpr static T value = v;
};

using true_type = integral_constant<bool, true>;
using false_type = integral_constant<bool, false>;

using BoolSet = TypeList<true_type, false_type>;

template <typename IN, template <typename> class F>
struct Map;

// 输入类型参数IN 和元函数 F (注意这里是元函数)
// 声明一个元函数Map
// 模式匹配当IN类型为TypeList时，对其每一个Ts...元素进行元函数F调用
// 最后将结果展开
template <template <typename> class F, typename... Ts>
struct Map<TypeList<Ts...>, F> {
  using type = TypeList<typename F<Ts>::type...>;
};

// 输入类型参数IN，和谓词函数P
// 默认返回类型为空的TypeList；列表为空时递归终止，返回当前OUT
template <typename IN, template <typename> class P, typename OUT = TypeList<>>
struct Filter {
  using type = OUT;
};

// 对当前列表第一个参数H进行P函数调用，根据真假判断要不要把结果放到OUT中
// 使用继承的方式省去了写using type = ...
template <template <typename> class P, typename OUT, typename H, typename... Ts>
struct Filter<TypeList<H, Ts...>, P, OUT>
    : std::conditional_t<
          P<H>::value,
          Filter<TypeList<Ts...>, P, typename OUT::template addend<H>>,
          Filter<TypeList<Ts...>, P, OUT>> {};

template <typename IN, template <typename> class P>
using Filter_t = typename Filter<IN, P>::type;

// 输入类型参数IN，初始类型参数INIT，二元函数OP
// 默认返回初始值；列表为空时递归终止返回当前INIT参数
template <typename IN, typename INIT, template <typename, typename> class OP>
struct Fold {
  using type = INIT;
};

template <typename IN, typename INIT, template <typename, typename> class OP>
using Fold_t = typename Fold<IN, INIT, OP>::type;

// 对当前参数H执行二元函数OP，其返回值类型更新INIT参数
template <typename ACC, template <typename, typename> class OP, typename H,
          typename... Ts>
struct Fold<TypeList<H, Ts...>, ACC, OP>
    : Fold<TypeList<Ts...>, typename OP<ACC, H>::type, OP> {};

// 连接两个TypeList
// 输入两个TypeList IN1, IN2
// 定义Append二元操作，输入两个参数，一个ACC TypeList，一个类参数E，通过调用TypeList的 a
// pend原函数
template <typename IN1, typename IN2>
class Concat {
  template <typename ACC, typename E>
  struct Append : ACC::template append<E> {};

 public:
  using type = Fold_t<IN2, IN1, Append>;
};

// Concat_t<TypeList<int, char>, TypeList<float>> ==> TypeList<int, char, float>
template <typename IN1, typename IN2>
using Concat_t = typename Concat<IN1, IN2>::type;

template <typename IN1, typename IN2>
struct Concat2;

template <typename... Ts1, typename... Ts2>
struct Concat2<TypeList<Ts1...>, TypeList<Ts2...>> {
  using type = TypeList<Ts1..., Ts2...>;
};

template <typename IN1, typename IN2>
using Concat2_t = typename Concat2<IN1, IN2>::type;

// 使用参数转发export_to，将IN2中的参数转发到IN的append函数上去
// 这里将export_to当作高阶函数，其输入一个函数IN::append,将自身的参数转调到这个元函数上
// 由于IN是模板类型参数，append又是模板元函数，需写成IN::template append
template <typename IN1, typename IN2>
struct Concat3 : IN2::template export_to<IN1::template append> {};

template <typename IN1, typename IN2>
using Concat3_t = typename Concat3<IN1, IN2>::type;

// 输入两个类型参数，IN TypeList，待查找类型E
// 定义二元炒作FindE,若ACC为真，则说明已经找到过，直接返回；否则判断当前类型参数是否与E相等
// Fold操作，输入IN TypeList，初始值类型为false_type，二元操作FindE
// 从布尔类型得到其值
template <typename IN, typename E>
class Elem {
  template <typename ACC, typename T>
  struct FindE : std::conditional_t<ACC::value, ACC, std::is_same<T, E>> {};
  using Found = Fold_t<IN, std::false_type, FindE>;

 public:
  constexpr static bool value = Found::value;
};

// Elem_v<TypeList<int>, int>; ==> true
// Elem_v<TypeList<int>, float>; ==> false
template <typename IN, typename E>
constexpr bool Elem_v = Elem<IN, E>::value;

}  // namespace detail
}  // namespace highkyck
