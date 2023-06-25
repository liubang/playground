//=====================================================================
//
// parallel_pipeline.cpp -
//
// Created by liubang on 2023/06/18 17:18
// Last Modified: 2023/06/18 17:18
//
//=====================================================================

#include <benchmark/benchmark.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_pipeline.h>

#include <cmath>
#include <iostream>
#include <vector>

struct Data {
  std::vector<float> arr;

  Data() {
    arr.resize(std::rand() % 100 * 500 + 10000);
    for (size_t i = 0; i < arr.size(); ++i) {
      arr[i] = std::rand() * (1.f / (float)RAND_MAX);
    }
  }

  void step1() {
    for (size_t i = 0; i < arr.size(); ++i) arr[i] += 3.14f;
  }

  void step2() {
    std::vector<float> tmp(arr.size());
    for (size_t i = 1; i < arr.size() - 1; ++i) {
      tmp[i] = arr[i - 1] + arr[i] + arr[i + 1];
    }
    std::swap(tmp, arr);
  }

  void step3() {
    for (size_t i = 0; i < arr.size(); ++i) {
      arr[i] = std::sqrt(std::abs(arr[i]));
    }
  }

  void step4() {
    std::vector<float> tmp(arr.size());
    for (size_t i = 1; i < arr.size() - 1; ++i) {
      tmp[i] = arr[i - 1] - 2 * arr[i] + arr[i + 1];
    }
    std::swap(tmp, arr);
  }
};

constexpr size_t n = 1 << 12;

static void test1(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<Data> datas(n);
    for (auto& data : datas) {
      data.step1();
      data.step2();
      data.step3();
      data.step4();
    }
  }
}

static void test2(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<Data> datas(n);
    tbb::parallel_for_each(datas.begin(), datas.end(), [&](Data& data) {
      data.step1();
      data.step2();
      data.step3();
      data.step4();
    });
  }
}

static void test3(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<Data> datas(n);
    tbb::parallel_for_each(datas.begin(), datas.end(),
                           [&](Data& data) { data.step1(); });

    tbb::parallel_for_each(datas.begin(), datas.end(),
                           [&](Data& data) { data.step2(); });

    tbb::parallel_for_each(datas.begin(), datas.end(),
                           [&](Data& data) { data.step3(); });

    tbb::parallel_for_each(datas.begin(), datas.end(),
                           [&](Data& data) { data.step4(); });
  }
}

static void test4(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<Data> datas(n);
    auto it = datas.begin();
    tbb::parallel_pipeline(
        16,
        tbb::make_filter<void, Data*>(tbb::filter_mode::serial_in_order,
                                      [&](tbb::flow_control& fc) -> Data* {
                                        if (it == datas.end()) {
                                          fc.stop();
                                          return nullptr;
                                        }
                                        return &*it++;
                                      }),
        tbb::make_filter<Data*, Data*>(tbb::filter_mode::parallel,
                                       [&](Data* data) -> Data* {
                                         data->step1();
                                         return data;
                                       }),
        tbb::make_filter<Data*, Data*>(tbb::filter_mode::parallel,
                                       [&](Data* data) -> Data* {
                                         data->step2();
                                         return data;
                                       }),
        tbb::make_filter<Data*, Data*>(tbb::filter_mode::parallel,
                                       [&](Data* data) -> Data* {
                                         data->step3();
                                         return data;
                                       }),
        tbb::make_filter<Data*, void>(tbb::filter_mode::parallel,
                                      [&](Data* data) -> Data* {
                                        data->step4();
                                        return data;
                                      }));
  }
}

BENCHMARK(test1);
BENCHMARK(test2);
BENCHMARK(test3);
BENCHMARK(test4);
