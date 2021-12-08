#pragma once
#include <functional>
#include <type_traits>

namespace highkyck {
namespace detail {

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
    , function_(std::forward<fn>(fn))
  {
    failsafe.dismiss();
  }

private:
  FunctionType function_;
};

enum class ScopeGuardOnExit
{
};


template<typename FunctionType>
ScopeGuardImpl<typename std::decay<FunctionType>::type, true> operator+(detail::ScopeGuardOnExit,
                                                                        FunctionType&& fn)
{}


}  // namespace detail
}  // namespace highkyck
