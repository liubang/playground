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
#include <tbb/parallel_sort.h>

#include <algorithm>
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
