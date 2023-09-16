//=====================================================================
//
// parallel_range2d.cpp -
//
// Created by liubang on 2023/06/18 00:27
// Last Modified: 2023/06/18 00:27
//
//=====================================================================
#include <nanobench.h>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>

#include <cmath>
#include <iostream>
#include <vector>

constexpr std::size_t n = 1 << 13;

static void test1() {
    std::vector<float> a(n * n);
    tbb::parallel_for(tbb::blocked_range2d<std::size_t>(0, n, 0, n),
                      [&](tbb::blocked_range2d<std::size_t> r) {
                          for (std::size_t i = r.cols().begin(); i < r.cols().end(); ++i) {
                              for (std::size_t j = r.rows().begin(); j < r.rows().end(); ++j) {
                                  a[i * n + j] = std::sin(i) * std::sin(j);
                              }
                          }
                      });
}

int main(int argc, char *argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    return 0;
}
