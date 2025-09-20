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

int main(int argc, char* argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    return 0;
}
