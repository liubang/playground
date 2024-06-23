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

#include "cpp/pl/hash/murmurhash2.h"

#include <gtest/gtest.h>

TEST(hash, murmurhash2_test) {
    const char* data = "hello world";
    uint64_t seed = 0;
    pl::CMurmurHash64 hasher;
    for (uint32_t i = 0; i < 4; ++i) {
        hasher.begin(seed);
        hasher.add(data, strlen(data), false);
        seed = hasher.end();
    }

    EXPECT_NE(0, seed);
}
