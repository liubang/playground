// Copyright (c) 2025 The Authors. All rights reserved.
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

#include "cpp/pl/fast/digit_count.h"
#include <benchmark/benchmark.h>

static void BM_digit_count(benchmark::State& state) {
    const uint64_t numbers = state.range(0);
    for (auto _ : state) {
        for (uint64_t i = 0; i < numbers; ++i) {
            benchmark::DoNotOptimize(pl::digit_count(i));
        }
        state.SetItemsProcessed(numbers);
    }
}

BENCHMARK(BM_digit_count)->Range(1 << 16, 1 << 20);

static void BM_alternative_digit_count(benchmark::State& state) {
    const uint64_t numbers = state.range(0);
    for (auto _ : state) {
        for (uint64_t i = 0; i < numbers; ++i) {
            benchmark::DoNotOptimize(pl::alternative_digit_count(i));
        }
        state.SetItemsProcessed(numbers);
    }
}
BENCHMARK(BM_alternative_digit_count)->Range(1 << 16, 1 << 20);

static void BM_fast_digit_count(benchmark::State& state) {
    const uint64_t numbers = state.range(0);
    for (auto _ : state) {
        for (uint64_t i = 0; i < numbers; ++i) {
            benchmark::DoNotOptimize(pl::fast_digit_count(i));
        }
        state.SetItemsProcessed(numbers);
    }
}
BENCHMARK(BM_fast_digit_count)->Range(1 << 16, 1 << 20);
