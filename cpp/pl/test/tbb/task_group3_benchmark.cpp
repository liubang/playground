// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#include <cmath>
#include <iostream>
#include <nanobench.h>
#include <tbb/task_group.h>
#include <vector>

constexpr size_t n = 1 << 26;

static void test1() {
    std::vector<float> a(n);
    float res = 0;
    for (size_t i = 0; i < n; ++i) {
        res += std::sin(i);
        a[i] = res;
    }

    std::cout << a[n / 2] << std::endl;
    std::cout << res << std::endl;
}

static void test2() {
    std::vector<float> a(n);
    float res = 0;
    size_t maxt = 16; // 当前系统的逻辑核数量
    tbb::task_group tg1;
    std::vector<float> tmp_res(maxt);
    for (size_t t = 0; t < maxt; ++t) {
        size_t beg = t * n / maxt;
        size_t end = std::min(n, (t + 1) * n / maxt);
        tg1.run([&, t, beg, end] {
            float local_res = 0;
            for (size_t i = beg; i < end; ++i) {
                local_res += std::sin(i);
            }
            tmp_res[t] = local_res;
        });
    }
    tg1.wait();
    for (size_t t = 0; t < maxt; ++t) {
        tmp_res[t] += res;
        res = tmp_res[t];
    }
    tbb::task_group tg2;
    for (size_t t = 0; t < maxt; ++t) {
        size_t beg = t * n / maxt - 1;
        size_t end = std::min(n, (t + 1) * n / maxt) - 1;
        tg2.run([&, t, beg, end] {
            float local_res = tmp_res[t];
            for (size_t i = beg; i < end; ++i) {
                local_res += std::sin(i);
                a[i] = local_res;
            }
        });
    }
    tg2.wait();

    std::cout << a[n / 2] << std::endl;
    std::cout << res << std::endl;
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
