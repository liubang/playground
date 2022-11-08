#pragma once

#include <type_traits>

namespace highkyck {
namespace meta {

// template <typename F, typename... Args>
// using InvokeResultOfFunc = decltype(F{}(Args{}...));

template <typename F, typename... Args>
using InvokeResultOfFunc = decltype(std::declval<F>()(std::declval<Args>()...));

}  // namespace meta
}  // namespace highkyck
