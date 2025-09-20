// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

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
