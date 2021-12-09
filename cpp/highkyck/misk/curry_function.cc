#include <iostream>
#include <tuple>
#include <utility>

namespace std {
struct nonesuch {
  nonesuch() = delete;
  ~nonesuch() = delete;
  nonesuch(const nonesuch&) = delete;
  void operator=(const nonesuch&) = delete;
};

template <class Default, class AlwaysVoid, template <class...> class Op,
          class... Args>
struct detector {
  using value_t = std::false_type;
  using type = Default;
};

template <class Default, template <class...> class Op, class... Args>
struct detector<Default, std::void_t<Op<Args...>>, Op, Args...> {
  using value_t = std::true_type;
  using type = Op<Args...>;
};

template <template <class...> class Op, class... Args>
using is_detected = typename detector<nonesuch, void, Op, Args...>::value_t;
}  // namespace std

namespace {
template <class T, typename... Args>
using can_invoke_t = decltype(std::declval<T>()(std::declval<Args>()...));

template <typename T, typename... Args>
using can_invoke = std::is_detected<can_invoke_t, T, Args...>;

template <typename F, typename... Arguments>
struct curry_t {
  template <typename... Args>
  constexpr decltype(auto) operator()(Args&&... a) const {
    curry_t<F, Arguments..., Args...> cur = {
        f_, std::tuple_cat(args_, std::make_tuple(std::forward<Args>(a)...))};

    if constexpr (!can_invoke<F, Arguments..., Args...>::value) {
      return cur;
    } else {
      return cur();
    }
  }

  constexpr decltype(auto) operator()() const { return std::apply(f_, args_); }

  F f_;
  std::tuple<Arguments...> args_;
};

template <typename F>
constexpr curry_t<F> curry(F&& f) {
  return {std::forward<F>(f)};
}
}  // namespace

int test(int a, int b, int c) { return 100 * a + 10 * b + c; }

int main(int argc, char* argv[]) {
  auto f = curry(test)(1);
  auto g = f(2);
  auto result = g(3);
  std::cout << result << '\n';
  auto result1 = curry(test)(1)(2)(3);
  std::cout << result1 << std::endl;
  return 0;
}
