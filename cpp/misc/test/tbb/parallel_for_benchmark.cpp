//=====================================================================
//
// parallel_for.cpp -
//
// Created by liubang on 2023/06/18 00:18
// Last Modified: 2023/06/18 00:18
//
//=====================================================================
#include <benchmark/benchmark.h>
#include <tbb/parallel_for.h>

#include <cmath>
#include <iostream>
#include <vector>

constexpr std::size_t n = 1 << 26;

static void test1(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<float> a(n);
    for (std::size_t i = 0; i < n; ++i) {
      a[i] = std::sin(i);
    }
  }
}

static void test2(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<float> a(n);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n),
                      [&](tbb::blocked_range<std::size_t> r) {
                        for (std::size_t i = r.begin(); i < r.end(); ++i) {
                          a[i] = std::sin(i);
                        }
                      });
  }
}

BENCHMARK(test1);
BENCHMARK(test2);
