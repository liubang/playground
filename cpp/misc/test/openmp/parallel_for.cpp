//=====================================================================
//
// parallel_for.cpp -
//
// Created by liubang on 2023/06/19 23:43
// Last Modified: 2023/06/19 23:43
//
//=====================================================================

#include <cmath>
#include <iostream>
#include <vector>

#include "cpp/tools/measure.h"

int main(int argc, char *argv[]) {
  std::size_t n = 1 << 26;
  pl::measure([n] {
    std::vector<float> a(n);
    for (std::size_t i = 0; i < n; ++i) {
      a[i] = std::sin(i);
    }
  });

  pl::measure([n] {
    std::vector<float> a(n);

#pragma omp parallel for
    for (std::size_t i = 0; i < n; ++i) {
      a[i] = std::sin(i);
    }
  });
  return 0;
}
