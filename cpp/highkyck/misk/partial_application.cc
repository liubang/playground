#include <tuple>
#include <iostream>

namespace {
template<typename F, typename... Args> class partial_t
{
public:
  constexpr partial_t(F&& f, Args&&... args)
      : f_(std::forward<F>(f))
      , args_(std::forward_as_tuple(args...))
  {}
  template<typename... RestArgs> constexpr decltype(auto) operator()(RestArgs&&... rest_args)
  {
    return std::apply(
        f_, std::tuple_cat(args_, std::forward_as_tuple(std::forward<RestArgs>(rest_args)...)));
  }

private:
  F f_;
  std::tuple<Args...> args_;
};

template<typename Fn, typename... Args> constexpr decltype(auto) partial(Fn&& fn, Args&&... args)
{
  return partial_t<Fn, Args...>(std::forward<Fn>(fn), std::forward<Args>(args)...);
}

};   // namespace

int test(int x, int y, int z)
{
  return x + y + z;
}

int main(int argc, char* argv[])
{
  auto f = partial(test, 5, 3);
  auto r = f(7);
  std::cout << r << '\n';

  auto r1 = partial(test)(5, 6, 7);
  std::cout << r1 << '\n';
  auto r2 = partial(test, 5)(6, 7);
  std::cout << r2 << '\n';
  auto r3 = partial(test, 5, 6)(7);
  std::cout << r3 << '\n';

  std::cout << std::endl;
  return 0;
}
