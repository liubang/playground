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

}  // namespace detail
}  // namespace highkyck
