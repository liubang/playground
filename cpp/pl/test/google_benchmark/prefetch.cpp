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

#include "cpp/pl/lang/common.h"

#include <benchmark/benchmark.h>
#include <vector>

// Function without __builtin_prefetch
void NoPrefetch(benchmark::State& state) {
    // Create a large vector to iterate over
    std::vector<int> data(state.range(0), 1);
    for (auto _ : state) {
        long sum = 0;
        for (const auto& i : data) {
            sum += i;
        }
        // Prevent compiler optimization to discard the sum
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(NoPrefetch)->Arg(1 << 20); // Run with 1MB of data (2^20 integers)

// Function with __builtin_prefetch
void WithPrefetch(benchmark::State& state) {
    // Create a large vector to iterate over
    std::vector<int> data(state.range(0), 1);
    for (auto _ : state) {
        long sum = 0;
        int prefetch_distance = 10;
        for (int i = 0; i < data.size(); i++) {
            if (i + prefetch_distance < data.size()) {
                PL_PREFETCH(&data[i + prefetch_distance], 0, 3);
            }
            sum += data[i];
        }
        // Prevent compiler optimization to discard the sum
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(WithPrefetch)->Arg(1 << 20); // Run with 1MB of data (2^20 integers)

BENCHMARK_MAIN();
