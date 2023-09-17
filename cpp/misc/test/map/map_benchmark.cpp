#include <benchmark/benchmark.h>

#include <iostream>
#include <map>
#include <random>
#include <unordered_map>

static void BM_MapInsert(benchmark::State &state) {
    const int num_elements = state.range(0);
    std::map<int, int> ordered_map;
    std::vector<int> keys(num_elements);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1, num_elements * 10);

    for (auto _ : state) {
        for (int i = 0; i < num_elements; ++i) {
            keys[i] = dist(gen);
        }

        for (int i = 0; i < num_elements; ++i) {
            ordered_map[keys[i]] = i;
        }

        state.PauseTiming(); // 暂停计时以准备下一个迭代
        ordered_map.clear();
        state.ResumeTiming(); // 恢复计时
    }
}
BENCHMARK(BM_MapInsert)->Range(8, 8 << 10);

static void BM_UnorderedMapInsert(benchmark::State &state) {
    const int num_elements = state.range(0);
    std::unordered_map<int, int> unordered_map;
    std::vector<int> keys(num_elements);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1, num_elements * 10);

    for (auto _ : state) {
        for (int i = 0; i < num_elements; ++i) {
            keys[i] = dist(gen);
        }

        for (int i = 0; i < num_elements; ++i) {
            unordered_map[keys[i]] = i;
        }

        state.PauseTiming(); // 暂停计时以准备下一个迭代
        unordered_map.clear();
        state.ResumeTiming(); // 恢复计时
    }
}
BENCHMARK(BM_UnorderedMapInsert)->Range(8, 8 << 10);

static void BM_MapRead(benchmark::State &state) {
    const int num_elements = state.range(0);
    std::map<int, int> ordered_map;
    std::vector<int> keys(num_elements);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1, num_elements * 10);

    for (int i = 0; i < num_elements; ++i) {
        keys[i] = dist(gen);
        ordered_map[keys[i]] = i;
    }

    for (auto _ : state) {
        for (int i = 0; i < num_elements; ++i) {
            int value = ordered_map[keys[i]];
            benchmark::DoNotOptimize(value); // 防止编译器优化
        }
    }
}
BENCHMARK(BM_MapRead)->Range(8, 8 << 10);

static void BM_UnorderedMapRead(benchmark::State &state) {
    const int num_elements = state.range(0);
    std::unordered_map<int, int> unordered_map;
    std::vector<int> keys(num_elements);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1, num_elements * 10);

    for (int i = 0; i < num_elements; ++i) {
        keys[i] = dist(gen);
        unordered_map[keys[i]] = i;
    }

    for (auto _ : state) {
        for (int i = 0; i < num_elements; ++i) {
            int value = unordered_map[keys[i]];
            benchmark::DoNotOptimize(value); // 防止编译器优化
        }
    }
}
BENCHMARK(BM_UnorderedMapRead)->Range(8, 8 << 10);
