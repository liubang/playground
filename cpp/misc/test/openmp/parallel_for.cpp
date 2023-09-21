//=====================================================================
//
// parallel_for.cpp -
//
// Created by liubang on 2023/06/19 23:43
// Last Modified: 2023/06/19 23:43
//
//=====================================================================

#include <nanobench.h>

#include <cmath>
#include <iostream>
#include <vector>

static constexpr std::size_t n = 1 << 26;

static void test1() {
    std::vector<float> a(n);
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = std::sin(i);
    }
    ankerl::nanobench::doNotOptimizeAway(a);
}

static void test2() {
    std::vector<float> a(n);

#pragma omp parallel for
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = std::sin(i);
    }

    ankerl::nanobench::doNotOptimizeAway(a);
}

int main(int argc, char* argv[]) {
    // put your code here
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    ankerl::nanobench::Bench().run("test2", [&] {
        test2();
    });
    return 0;
}
