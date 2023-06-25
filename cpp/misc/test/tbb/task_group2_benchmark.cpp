//=====================================================================
//
// task_group2.cpp -
//
// Created by liubang on 2023/06/18 01:18
// Last Modified: 2023/06/18 01:18
//
//=====================================================================
#include <benchmark/benchmark.h>
#include <tbb/task_group.h>

#include <cmath>
#include <iostream>
#include <vector>

constexpr std::size_t n = 1 << 26;

static void test1(benchmark::State& state) {
  for (auto _ : state) {
    float res = 0;
    std::size_t maxt = 4;
    tbb::task_group tg;
    std::vector<float> tmp_res(maxt);
    for (std::size_t t = 0; t < maxt; ++t) {
      std::size_t beg = t * n / maxt;
      std::size_t end = std::min(n, (t + 1) * n / maxt);
      tg.run([&, t, beg, end] {
        float local_res = 0;
        for (std::size_t i = beg; i < end; ++i) {
          local_res += std::sin(i);
        }
        tmp_res[t] = local_res;
      });
    }
    tg.wait();
    for (std::size_t t = 0; t < maxt; ++t) {
      res += tmp_res[t];
    }
  }
}

BENCHMARK(test1);
