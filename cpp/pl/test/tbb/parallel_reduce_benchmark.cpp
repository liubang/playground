//=====================================================================
//
// parallel_reduce.cpp -
//
// Created by liubang on 2023/06/18 12:59
// Last Modified: 2023/06/18 12:59
//
//=====================================================================
#include <nanobench.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_reduce.h>

#include <cmath>
#include <iostream>
#include <vector>

constexpr size_t n = 1 << 10;

static void test1() {
    float res = 0;
    for (size_t i = 0; i < n; ++i) {
        res += std::sin(i);
    }
    // std::cout << res << std::endl;
}

static void test2() {
    float res = tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, n), (float)0,
        [&](tbb::blocked_range<size_t> r, float local_res) {
            for (size_t i = r.begin(); i < r.end(); ++i) {
                local_res += std::sin(i);
            }
            return local_res;
        },
        [](float x, float y) {
            return x + y;
        });

    // std::cout << res << std::endl;
}

int main(int argc, char* argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    ankerl::nanobench::Bench().run("test2", [&] {
        test2();
    });
    return 0;
}
