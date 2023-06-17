//=====================================================================
//
// parallel_range2d.cpp -
//
// Created by liubang on 2023/06/18 00:27
// Last Modified: 2023/06/18 00:27
//
//=====================================================================
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>

#include <cmath>
#include <iostream>
#include <vector>

#include "cpp/misc/test/tbb/tools.h"

int main(int argc, char *argv[]) {
  pl::measure([] {
    std::size_t n = 1 << 13;
    std::vector<float> a(n * n);
    tbb::parallel_for(
        tbb::blocked_range2d<std::size_t>(0, n, 0, n),
        [&](tbb::blocked_range2d<std::size_t> r) {
          for (std::size_t i = r.cols().begin(); i < r.cols().end(); ++i) {
            for (std::size_t j = r.rows().begin(); j < r.rows().end(); ++j) {
              a[i * n + j] = std::sin(i) * std::sin(j);
            }
          }
        });
  });
  return 0;
}
