//=====================================================================
//
// task_group3.cpp -
//
// Created by liubang on 2023/06/18 13:13
// Last Modified: 2023/06/18 13:13
//
//=====================================================================
#include <tbb/task_group.h>

#include <cmath>
#include <iostream>
#include <vector>

#include "cpp/tools/measure.h"

auto main(int argc, char *argv[]) -> int {
  size_t n = 1 << 26;
  pl::measure([n] {
    std::vector<float> a(n);
    float res = 0;
    for (size_t i = 0; i < n; ++i) {
      res += std::sin(i);
      a[i] = res;
    }

    std::cout << a[n / 2] << std::endl;
    std::cout << res << std::endl;
  });

  pl::measure([n] {
    std::vector<float> a(n);
    float res = 0;
    size_t maxt = 16;  // 当前系统的逻辑核数量
    tbb::task_group tg1;
    std::vector<float> tmp_res(maxt);
    for (size_t t = 0; t < maxt; ++t) {
      size_t beg = t * n / maxt;
      size_t end = std::min(n, (t + 1) * n / maxt);
      tg1.run([&, t, beg, end] {
        float local_res = 0;
        for (size_t i = beg; i < end; ++i) {
          local_res += std::sin(i);
        }
        tmp_res[t] = local_res;
      });
    }
    tg1.wait();
    for (size_t t = 0; t < maxt; ++t) {
      tmp_res[t] += res;
      res = tmp_res[t];
    }
    tbb::task_group tg2;
    for (size_t t = 0; t < maxt; ++t) {
      size_t beg = t * n / maxt - 1;
      size_t end = std::min(n, (t + 1) * n / maxt) - 1;
      tg2.run([&, t, beg, end] {
        float local_res = tmp_res[t];
        for (size_t i = beg; i < end; ++i) {
          local_res += std::sin(i);
          a[i] = local_res;
        }
      });
    }
    tg2.wait();

    std::cout << a[n / 2] << std::endl;
    std::cout << res << std::endl;
  });
  return 0;
}
