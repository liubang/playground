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
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>

#include <cmath>
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

int main(int argc, char* argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    ankerl::nanobench::Bench().run("test2", [&] {
        test2();
    });
    return 0;
}
