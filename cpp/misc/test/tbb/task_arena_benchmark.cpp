//=====================================================================
//
// task_arena.cpp -
//
// Created by liubang on 2023/06/18 14:06
// Last Modified: 2023/06/18 14:06
//
//=====================================================================
#include <benchmark/benchmark.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <cmath>
#include <iostream>
#include <vector>

constexpr size_t n = 1 << 26;

static void test1(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<float> a(n);
    tbb::task_arena ta;
    ta.execute([&] {
      tbb::parallel_for((size_t)0, (size_t)n,
                        [&](size_t i) { a[i] = std::sin(i); });
    });
  }
}

BENCHMARK(test1);
