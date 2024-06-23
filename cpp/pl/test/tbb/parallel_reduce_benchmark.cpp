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

#include <iostream>
#include <nanobench.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_reduce.h>

#include <cmath>

constexpr size_t n = 1 << 10;

static void test1() {
    float res = 0;
    for (size_t i = 0; i < n; ++i) {
        res += std::sin(i);
    }
    std::cout << res << std::endl;
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
