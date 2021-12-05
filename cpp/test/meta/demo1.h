#pragma once

namespace meta {
template<int a>
constexpr int fun = a + 1;

template<class T>
struct Fun_
{
  using type = T;
};

template<class T>
using Fun_t = typename Fun_<T>::type;

template<>
struct Fun_<int>
{
  using type = unsigned int;
};

template<>
struct Fun_<long>
{
  using type = unsigned long;
};

}  // namespace meta
