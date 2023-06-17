//=====================================================================
//
// parallel_for.cpp -
//
// Created by liubang on 2023/06/18 00:18
// Last Modified: 2023/06/18 00:18
//
//=====================================================================
#include <tbb/parallel_for.h>

#include <cmath>
#include <iostream>
#include <vector>

#include "cpp/misc/test/tbb/tools.h"

int main(int argc, char *argv[]) {
  pl::measure([] {
    std::size_t n = 1 << 26;
    std::vector<float> a(n);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n),
                      [&](tbb::blocked_range<std::size_t> r) {
                        for (std::size_t i = r.begin(); i < r.end(); ++i) {
                          a[i] = std::sin(i);
                        }
                      });
  });
  return 0;
}
