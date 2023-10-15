//=====================================================================
//
// 01.cpp -
//
// Created by liubang on 2023/10/15 23:36
// Last Modified: 2023/10/15 23:36
//
//=====================================================================

#include <benchmark/benchmark.h>

constexpr size_t n = 1 << 26;

static void write_0(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<int> arr(n);
        for (size_t i = 0; i < n; ++i) {
            arr[i] = 0;
        }
        benchmark::DoNotOptimize(arr);
    }
}

BENCHMARK(write_0);

static void write_1(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<int> arr(n);
        for (size_t i = 0; i < n; ++i) {
            arr[i] = 1;
        }
        benchmark::DoNotOptimize(arr);
    }
}

BENCHMARK(write_1);
