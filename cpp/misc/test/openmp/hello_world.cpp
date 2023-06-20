//=====================================================================
//
// hello_world.cpp -
//
// Created by liubang on 2023/06/19 23:40
// Last Modified: 2023/06/19 23:40
//
//=====================================================================

#include <benchmark/benchmark.h>

#include <iostream>

static void hello(benchmark::State& state) {
  for (auto _ : state) {
#pragma omp parallel
    { std::cout << "hello world" << std::endl; }
  }
}

BENCHMARK(hello);
