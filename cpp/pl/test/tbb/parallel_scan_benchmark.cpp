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

#include <nanobench.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_scan.h>

#include <cmath>
#include <iostream>
#include <vector>

constexpr size_t n = 1 << 26;

static void test1() {
    std::vector<float> a(n);
    float res = tbb::parallel_scan(
        tbb::blocked_range<size_t>(0, n), (float)0,
        [&](tbb::blocked_range<size_t> r, float local_res, auto is_final) {
            for (size_t i = r.begin(); i < r.end(); ++i) {
                local_res += std::sin(i);
                if (is_final) {
                    a[i] = local_res;
                }
            }
            return local_res;
        },
        [](float x, float y) {
            return x + y;
        });

    std::cout << a[n / 2] << std::endl;
    std::cout << res << std::endl;
}

int main(int argc, char* argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    return 0;
}
