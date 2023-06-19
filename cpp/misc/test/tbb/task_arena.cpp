//=====================================================================
//
// task_arena.cpp -
//
// Created by liubang on 2023/06/18 14:06
// Last Modified: 2023/06/18 14:06
//
//=====================================================================
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <cmath>
#include <iostream>
#include <vector>

#include "cpp/tools/measure.h"

int main(int argc, char *argv[]) {
  size_t n = 1 << 26;
  pl::measure([n] {
    std::vector<float> a(n);
    tbb::task_arena ta;
    ta.execute([&] {
      tbb::parallel_for((size_t)0, (size_t)n,
                        [&](size_t i) { a[i] = std::sin(i); });
    });
  });
  return 0;
}
