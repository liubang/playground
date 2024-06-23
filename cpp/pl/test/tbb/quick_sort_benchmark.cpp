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

#include <algorithm>
#include <vector>

#include "cpp/pl/test/tbb/quick_sort.h"

constexpr size_t n = 1 << 10;

static void test1() {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    std::sort(arr.begin(), arr.end(), std::less<int>{});
}

static void test2() {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    pl::quick_sort(arr.data(), arr.size());
}

static void test3() {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    pl::quick_sort2(arr.data(), arr.size());
}

static void test4() {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    pl::quick_sort3(arr.data(), arr.size());
}

int main(int argc, char* argv[]) {
    ankerl::nanobench::Bench().run("test1", [&] {
        test1();
    });
    ankerl::nanobench::Bench().run("test2", [&] {
        test2();
    });
    ankerl::nanobench::Bench().run("test3", [&] {
        test3();
    });
    ankerl::nanobench::Bench().run("test4", [&] {
        test4();
    });
    return 0;
}
