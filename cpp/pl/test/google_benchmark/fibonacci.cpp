#include <benchmark/benchmark.h>
#include <cstdint>

std::uint64_t Fibonacci(std::uint64_t number) {
    return number < 2 ? 1 : Fibonacci(number - 1) + Fibonacci(number - 2);
}

static void BM_StringCreation(benchmark::State& state) {
    for (auto _ : state) {
        Fibonacci(20);
    }
}

// Register the function as a benchmark
BENCHMARK(BM_StringCreation);
