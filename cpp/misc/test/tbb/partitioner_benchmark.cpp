//=====================================================================
//
// partitioner.cpp -
//
// Created by liubang on 2023/06/18 14:20
// Last Modified: 2023/06/18 14:20
//
//=====================================================================
#include <nanobench.h>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>

#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

constexpr size_t n = 1 << 14;

static void test1() {
    std::vector<float> a(n * n);
    std::vector<float> b(n * n);

    tbb::parallel_for(tbb::blocked_range2d<size_t>(0, n, 0, n),
                      [&](tbb::blocked_range2d<size_t> r) {
                          for (size_t i = r.cols().begin(); i < r.cols().end(); ++i) {
                              for (size_t j = r.rows().begin(); j < r.rows().end(); ++j) {
                                  b[i * n + j] = a[j * n + i];
                              }
                          }
                      });
}

static void test2() {
    std::vector<float> a(n * n);
    std::vector<float> b(n * n);
    size_t grain = 24;
    tbb::parallel_for(
        tbb::blocked_range2d<size_t>(0, n, grain, 0, n, grain),
        [&](tbb::blocked_range2d<size_t> r) {
            for (size_t i = r.cols().begin(); i < r.cols().end(); ++i) {
                for (size_t j = r.rows().begin(); j < r.rows().end(); ++j) {
                    b[i * n + j] = a[j * n + i];
                }
            }
        },
        tbb::simple_partitioner{});
}

int main(int argc, char *argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    ankerl::nanobench::Bench().run("test2", [&] {
        test2();
    });
    return 0;
}
