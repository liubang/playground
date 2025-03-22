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
#include <random>

// 生成随机数进行测试
std::vector<int64_t> generate_test_cases(int count) {
    std::vector<int64_t> test_cases(count);
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<int64_t> dis(0, INT64_MAX);

    for (int i = 0; i < count; ++i) {
        test_cases[i] = dis(gen);
    }

    return test_cases;
}

static void BM_digit_count(benchmark::State& state) {
    const uint64_t numbers = state.range(0);
    auto test_cases = generate_test_cases(numbers);
    for (auto _ : state) {
        for (const auto& i : test_cases) {
            auto result = pl::digit_count(i);
            benchmark::DoNotOptimize(result);
        }
    }
    state.SetItemsProcessed(state.iterations() * numbers);
}

static void BM_alternative_digit_count(benchmark::State& state) {
    const uint64_t numbers = state.range(0);
    auto test_cases = generate_test_cases(numbers);
    for (auto _ : state) {
        for (const auto& i : test_cases) {
            auto result = pl::alternative_digit_count(i);
            benchmark::DoNotOptimize(result);
        }
    }
    state.SetItemsProcessed(state.iterations() * numbers);
}

static void BM_fast_digit_count(benchmark::State& state) {
    const uint64_t numbers = state.range(0);
    auto test_cases = generate_test_cases(numbers);
    for (auto _ : state) {
        for (const auto& i : test_cases) {
            auto result = pl::fast_digit_count(i);
            benchmark::DoNotOptimize(result);
        }
    }
    state.SetItemsProcessed(state.iterations() * numbers);
}

BENCHMARK(BM_digit_count)->Arg(100)->Arg(1000)->Arg(10000);
BENCHMARK(BM_alternative_digit_count)->Arg(100)->Arg(1000)->Arg(10000);
BENCHMARK(BM_fast_digit_count)->Arg(100)->Arg(1000)->Arg(10000);
