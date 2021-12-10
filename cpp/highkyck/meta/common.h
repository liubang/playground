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

template <template <typename> class F, typename... Ts>
struct Map<TypeList<Ts...>, F> {
  using type = TypeList<typename F<Ts>::type...>;
};

}  // namespace detail
}  // namespace highkyck
