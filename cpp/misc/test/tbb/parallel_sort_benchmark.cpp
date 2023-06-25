//=====================================================================
//
// parallel_sort.cpp -
//
// Created by liubang on 2023/06/18 17:08
// Last Modified: 2023/06/18 17:08
//
//=====================================================================
#include <benchmark/benchmark.h>
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <iostream>
#include <vector>

constexpr size_t n = 1 << 24;

static void test1(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    tbb::parallel_sort(arr.begin(), arr.end(), std::less<int>{});
  }
}

BENCHMARK(test1);
