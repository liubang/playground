#pragma once

#include <iostream>

namespace highkyck::meta {

void print1() {}

template <typename A, typename... Args>
void print1(const A& arg, const Args&... args) {
  std::cout << arg << std::endl;
  print1(args...);
}

template <typename... Args>
void print2(const Args&... args) {
  ((std::cout << args << std::endl), ...);
}

}  // namespace highkyck::meta
