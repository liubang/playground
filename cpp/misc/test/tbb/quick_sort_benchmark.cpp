//=====================================================================
//
// quick_sort.cpp -
//
// Created by liubang on 2023/06/18 16:59
// Last Modified: 2023/06/18 16:59
//
//=====================================================================
#include <benchmark/benchmark.h>

#include <algorithm>
#include <iostream>
#include <vector>

#include "cpp/misc/test/tbb/quick_sort.h"

constexpr size_t n = 1 << 24;

static void test1(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    std::sort(arr.begin(), arr.end(), std::less<int>{});
  }
}

static void test2(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    pl::quick_sort(arr.data(), arr.size());
  }
}

static void test3(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    pl::quick_sort2(arr.data(), arr.size());
  }
}

static void test4(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    pl::quick_sort3(arr.data(), arr.size());
  }
}

BENCHMARK(test1);
BENCHMARK(test2);
BENCHMARK(test3);
BENCHMARK(test4);
