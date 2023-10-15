//=====================================================================
//
// 02.cpp -
//
// Created by liubang on 2023/10/15 23:42
// Last Modified: 2023/10/15 23:42
//
//=====================================================================

#include <benchmark/benchmark.h>
#include <cstddef>
#include <vector>

constexpr size_t n = 1 << 28;
std::vector<float> a(n);

void BM_serial_add(benchmark::State& bm) {
    for (auto _ : bm) {
        for (size_t i = 0; i < n; ++i) {
            a[i] = a[i] + 1;
        }
        benchmark::DoNotOptimize(a);
    }
}

BENCHMARK(BM_serial_add);

void BM_parallel_add(benchmark::State& bm) {
    for (auto _ : bm) {
#pragma omp parallel for
        for (size_t i = 0; i < n; ++i) {
            a[i] = a[i] + 1;
        }
        benchmark::DoNotOptimize(a);
    }
}

BENCHMARK(BM_parallel_add);
