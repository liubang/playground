//=====================================================================
//
// tools.h -
//
// Created by liubang on 2023/06/18 00:32
// Last Modified: 2023/06/18 00:32
//
//=====================================================================
#pragma once

#include <chrono>
#include <iostream>
#include <type_traits>

namespace pl {

template <typename Fn,
          std::enable_if<std::is_invocable_v<typename std::decay<Fn>::type>,
                         void>::type* = nullptr>
void measure(Fn&& fn) {
  auto start = std::chrono::high_resolution_clock::now();
  fn();
  auto end = std::chrono::high_resolution_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  std::cout << ">> Elapsed time: " << elapsed.count() << "(ms)" << std::endl;
}

}  // namespace pl
