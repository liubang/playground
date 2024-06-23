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
