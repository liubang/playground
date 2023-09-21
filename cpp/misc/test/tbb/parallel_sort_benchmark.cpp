//=====================================================================
//
// parallel_sort.cpp -
//
// Created by liubang on 2023/06/18 17:08
// Last Modified: 2023/06/18 17:08
//
//=====================================================================
#include <nanobench.h>
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <iostream>
#include <vector>

constexpr size_t n = 1 << 24;

static void test1() {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    tbb::parallel_sort(arr.begin(), arr.end(), std::less<int>{});
}

int main(int argc, char* argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    return 0;
}
