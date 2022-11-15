#pragma once

#include <iostream>
#include <new>
#include <type_traits>
#include <utility>

namespace highkyck {
namespace detail {

struct T {
  enum { int_t, float_t } type;

  // a common mistack is to declare two funciton templates that differ only in
  // their default template arguments. This does not word because the
  // declarations are treated as redeclarations of the same function template.
  //
  // template <typename Integer,
  //           typename = std::enable_if<std::is_integral<Integer>::value>>
  // T(Integer) : type(int_t) {}
  //
  // template <typename Floating,
  //           typename =
  //           std::enable_if<std::is_floating_point<Floating>::value>>
  // T(Floating) : type(float_t) {}

  template <typename Integer,
            std::enable_if_t<std::is_integral<Integer>::value> = true>
  T(Integer) : type(int_t) {}

  template <typename Floating,
            std::enable_if_t<std::is_floating_point<Floating>::value> = true>
  T(Floating) : type(float_t) {}
};

}  // namespace detail

namespace detail {

void* voidify(const volatile void* ptr) noexcept {
  return const_cast<void*>(ptr);
}

// #1. enabled via the return type
template <class T>
typename std::enable_if<std::is_trivially_default_constructible<T>::value>::type
construct(T*) {
  std::cout << "default constructing trivially default construcible T\n";
}

// #2. using helper type
template <class T, class... Args>
std::enable_if<std::is_constructible<T, Args&&...>::value> construct(
    T* p,
    Args&&... args) {
  std::cout << "constructing T with operation\n";
  ::new (detail::voidify(p)) T(static_cast<Args&>(args)...);
}

// #3. enable via a parameter
template <class T>
void destroy(
    T*,
    typename std::enable_if<std::is_trivially_destructible<T>::value>::type* =
        0) {
  std::cout << "destroying trivially destructible T\n";
}

// #4. enable via a non-type template parameter
template <
    class T,
    typename std::enable_if<!std::is_trivially_destructible<T>{} &&
                                (std::is_class<T>{} || std::is_union<T>{}),
                            bool>::type = true>
void destroy(T* t) {
  std::cout << "destroying non-trivially destructible T\n";
  t->~T();
}

// #5. enabled via a type template parameter
template <class T, typename = std::enable_if<std::is_array<T>::value>>
void destroy(T* t) {
  for (std::size_t i = 0; i < std::extent<T>::value; ++i) {
    destroy((*t)[i]);
  }
}

}  // namespace detail
}  // namespace highkyck
