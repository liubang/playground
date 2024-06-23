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
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <cmath>
#include <vector>

constexpr size_t n = 1 << 26;

static void test1() {
    std::vector<float> a(n);
    tbb::task_arena ta;
    ta.execute([&] {
        tbb::parallel_for((size_t)0, (size_t)n, [&](size_t i) {
            a[i] = std::sin(i);
        });
    });
}

int main(int argc, char* argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    return 0;
}
