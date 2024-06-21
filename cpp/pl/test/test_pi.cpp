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

#include <cstddef>
#include <iostream>

double compute_pi_leibniz(size_t N) {
    double pi = 0.0;
    for (size_t i = 0; i < N; i++) {
        double tmp = (i & 1) ? (-1) : 1;
        pi += tmp / (2 * i + 1);
    }
    return pi * 4.0;
}

int main(int argc, char* argv[]) {
    for (int i = 10; i < 1024; ++i) {
        std::cout << compute_pi_leibniz(i) << '\n';
    }
    return 0;
}
