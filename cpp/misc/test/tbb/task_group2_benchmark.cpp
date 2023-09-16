//=====================================================================
//
// task_group2.cpp -
//
// Created by liubang on 2023/06/18 01:18
// Last Modified: 2023/06/18 01:18
//
//=====================================================================
#include <nanobench.h>
#include <tbb/task_group.h>

#include <cmath>
#include <iostream>
#include <vector>

constexpr std::size_t n = 1 << 26;

static void test1() {
    float res = 0;
    std::size_t maxt = 4;
    tbb::task_group tg;
    std::vector<float> tmp_res(maxt);
    for (std::size_t t = 0; t < maxt; ++t) {
        std::size_t beg = t * n / maxt;
        std::size_t end = std::min(n, (t + 1) * n / maxt);
        tg.run([&, t, beg, end] {
            float local_res = 0;
            for (std::size_t i = beg; i < end; ++i) {
                local_res += std::sin(i);
            }
            tmp_res[t] = local_res;
        });
    }
    tg.wait();
    for (std::size_t t = 0; t < maxt; ++t) {
        res += tmp_res[t];
    }
    std::cout << res << '\n';
}

int main(int argc, char *argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    return 0;
}
