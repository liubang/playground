#pragma once
#include <functional>
#include <type_traits>
#include <utility>

#include "common.h"

namespace highkyck {
namespace detail {

struct ScopeGuardDismissed
{};

class ScopeGuardImplBase
{
public:
  void dismiss() noexcept { dismissed_ = true; }
  void rehire() noexcept { dismissed_ = false; }

protected:
  ScopeGuardImplBase(bool dismissed = false) noexcept
    : dismissed_(dismissed)
  {}

  [[noreturn]] static void terminate() noexcept;

  static ScopeGuardImplBase make_empty_Scope_guard() noexcept { return ScopeGuardImplBase{}; }

  template<typename T>
  static const T& as_const(const T& t) noexcept
  {
    return t;
  }

  bool dismissed_;
};


template<typename FunctionType, bool InvokeNoexcept>
class ScopeGuardImpl : public ScopeGuardImplBase
{
public:
  explicit ScopeGuardImpl(FunctionType& fn) noexcept(
    std::is_nothrow_copy_constructible<FunctionType>::value)
    : ScopeGuardImpl(as_const(fn),
                     make_failsafe(std::is_nothrow_copy_constructible<FunctionType>{}, &fn))
  {}

  explicit ScopeGuardImpl(const FunctionType& fn) noexcept(
    std::is_nothrow_copy_constructible<FunctionType>::value)
    : ScopeGuardImpl(fn, make_failsafe(std::is_nothrow_copy_constructible<FunctionType>{}, &fn))
  {}

  explicit ScopeGuardImpl(FunctionType&& fn) noexcept(
    std::is_nothrow_move_constructible<FunctionType>::value)
    : ScopeGuardImpl(std::move_if_noexcept(fn),
                     make_failsafe(std::is_nothrow_move_constructible<FunctionType>{}, &fn))
  {}

  explicit ScopeGuardImpl(FunctionType&& fn, ScopeGuardDismissed) noexcept(
    std::is_nothrow_move_constructible<FunctionType>::value)
    : ScopeGuardImplBase{true}
    , function_(std::forward<FunctionType>(fn))
  {}

  ScopeGuardImpl(ScopeGuardImpl&& other) noexcept(
    std::is_nothrow_move_constructible<FunctionType>::value)
    : function_(std::move_if_noexcept(other.function_))
  {
    dismissed_ = std::exchange(other.dismissed_, true);
  }

  ~ScopeGuardImpl() noexcept(InvokeNoexcept)
  {
    if (!dismissed_) { execute(); }
  }

private:
  static ScopeGuardImplBase make_failsafe(std::true_type, const void*) noexcept
  {
    return make_empty_Scope_guard();
  }

  template<typename Fn>
  static auto make_failsafe(std::false_type, Fn* fn) noexcept
    -> ScopeGuardImpl<decltype(std::ref(*fn)), InvokeNoexcept>
  {
    return ScopeGuardImpl<decltype(std::ref(*fn)), InvokeNoexcept>{std::ref(*fn)};
  }


  template<typename Fn>
  explicit ScopeGuardImpl(Fn&& fn, ScopeGuardImplBase&& failsafe)
    : ScopeGuardImplBase{}
    , function_(std::forward<FunctionType>(fn))
  {
    failsafe.dismiss();
  }

  void* operator new(std::size_t) = delete;

  void execute() noexcept(InvokeNoexcept)
  {
    if (InvokeNoexcept) {
      // using R = decltype(function_());
      // auto catcher_word = reinterpret_cast<uintptr_t>(&terminate);
      // auto catcher = reinterpret_cast<R (*)()>(catcher_word);
      // catch_exception(function_, catcher);
      try {
        function_();
      } catch (...) {}
    } else {
      function_();
    }
  }

private:
  FunctionType function_;
};

template<typename F, bool INE>
using ScopeGuardImplDecay = ScopeGuardImpl<typename std::decay<F>::type, INE>;

enum class ScopeGuardOnExit
{
};


template<typename FunctionType>
ScopeGuardImpl<typename std::decay<FunctionType>::type, true> operator+(detail::ScopeGuardOnExit,
                                                                        FunctionType&& fn)
{
  return ScopeGuardImpl<typename std::decay<FunctionType>::type, true>(
    std::forward<FunctionType>(fn));
}

}  // namespace detail
}  // namespace highkyck


#define SCOPE_EXIT                              \
 auto HK_ANONYMOUS_VARIABLE(SCOPE_EXIT_STATE) = \
   ::highkyck::detail::ScopeGuardOnExit() + [&]() noexcept
