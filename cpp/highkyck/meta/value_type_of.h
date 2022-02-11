#pragma once

#include <type_traits>

namespace highkyck {
namespace meta {

template <typename T>
T type();

template <typename Container>
auto value_type_of_() {
  if constexpr (std::is_array<Container>::value) {
    return type<std::remove_extent_t<Container>>();
  } else {
    return type<typename Container::value_type>();
  }
}

template <typename Container>
using value_type_of = decltype(value_type_of_<Container>());

}  // namespace meta
}  // namespace highkyck
