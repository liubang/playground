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

#include "print.h"

#include <immintrin.h>

void t1() {
#ifdef __GNUC__
    float x = 0.5f;
    float y = 0.6f;
    float z = 0.7f;
    float w = 0.8f;

    __m128 m = {x, y, z, w};
    m += 1;
    pl::println_simd<__m128, 4>(m);
#endif
}

void t2() {
    float x = 0.5f;
    float y = 0.6f;
    float z = 0.7f;
    float w = 0.8f;

    // __m128 m = _mm_set_ps(w, z, y, x);
    __m128 m = _mm_setr_ps(x, y, z, w);
    __m128 one = _mm_set1_ps(1.0f);
    m = _mm_add_ps(m, one);
    pl::println_simd<__m128, 4>(m);
}

int main(int argc, char* argv[]) {
    t1();
    t2();
    return 0;
}
