//=====================================================================
//
// scope.h -
//
// Created by liubang on 2023/06/08 14:56
// Last Modified: 2023/06/08 14:56
//
//=====================================================================
#pragma once

#include <utility>

namespace playground::cpp::tools {

template <typename Fn>
class ScopeGuard {
 public:
  ScopeGuard(Fn&& fn) : fn_(std::forward<Fn>(fn)), enabled_(true) {}
  ~ScopeGuard() {
    if (enabled_)
      fn_();
  }

  void dismiss() { enabled_ = false; }

 private:
  Fn fn_;
  bool enabled_;
};

namespace detail {

struct ScopeOnExist {};
template <typename Fn>
inline ScopeGuard<Fn> operator+(ScopeOnExist, Fn&& fn) {
  return ScopeGuard<Fn>(std::forward<Fn>(fn));
}

}  // namespace detail

#define SCOPE_EXIT                           \
  auto __playground_cpp_tools_ScopeGuard__ = \
      playground::cpp::tools::detail::ScopeOnExist{} + [&]

#define CANCEL_SCOPE_EXIT __playground_cpp_tools_ScopeGuard__.dismiss()

}  // namespace playground::cpp::tools
