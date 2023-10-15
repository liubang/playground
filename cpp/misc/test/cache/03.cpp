//=====================================================================
//
// 03.cpp -
//
// Created by liubang on 2023/10/15 23:51
// Last Modified: 2023/10/15 23:51
//
//=====================================================================
#include <benchmark/benchmark.h>
#include <cstddef>
#include <vector>

constexpr size_t n = 1 << 28;
std::vector<float> a(n);

static float func(float x) { return x * (x * x + x * 3.14f - 1 / (x + 1)) + 42 / (2.718f - x); }

void BM_serial_func(benchmark::State& bm) {
    for (auto _ : bm) {
        for (size_t i = 0; i < n; ++i) {
            a[i] = func(a[i]);
        }
        benchmark::DoNotOptimize(a);
    }
}

BENCHMARK(BM_serial_func);

void BM_parallel_func(benchmark::State& bm) {
    for (auto _ : bm) {
#pragma omp parallel for
        for (size_t i = 0; i < n; ++i) {
            a[i] = func(a[i]);
        }
        benchmark::DoNotOptimize(a);
    }
}

BENCHMARK(BM_parallel_func);
