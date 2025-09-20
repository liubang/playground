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
#include <nanobench.h>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>
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

int main(int argc, char* argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    return 0;
}
