//=====================================================================
//
// parallel_for.cpp -
//
// Created by liubang on 2023/06/19 23:43
// Last Modified: 2023/06/19 23:43
//
//=====================================================================

#include <benchmark/benchmark.h>

#include <cmath>
#include <iostream>
#include <vector>

static constexpr std::size_t n = 1 << 26;

static void test1(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<float> a(n);
    for (std::size_t i = 0; i < n; ++i) {
      a[i] = std::sin(i);
    }
    benchmark::DoNotOptimize(a);
  }
}

static void test2(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<float> a(n);

#pragma omp parallel for
    for (std::size_t i = 0; i < n; ++i) {
      a[i] = std::sin(i);
    }

    benchmark::DoNotOptimize(a);
  }
}

BENCHMARK(test1);
BENCHMARK(test2);
