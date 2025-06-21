// Copyright (c) 2025 The Authors. All rights reserved.
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

#include <cstring>
#include <fmt/format.h>

namespace pl {

template <typename T, size_t N> void println_simd(const T& simd) {
    alignas(32) float data[N];
    std::memcpy(data, &simd, sizeof(T));
    fmt::print("[");
    for (size_t i = 0; i < N; ++i) {
        fmt::print("{}", data[i]);
        if (i + 1 < N) {
            fmt::print(", ");
        }
    }
    fmt::print("]\n");
}

} // namespace pl
