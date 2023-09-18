//=====================================================================
//
// murmurhash2_test.cc -
//
// Created by liubang on 2023/05/21 22:42
// Last Modified: 2023/05/21 22:42
//
//=====================================================================
#include "cpp/misc/hash/murmurhash2.h"

#include <gtest/gtest.h>

TEST(hash, murmurhash2_test) {
    const char* data = "hello world";
    uint64_t seed    = 0;
    pl::CMurmurHash64 hasher;
    for (uint32_t i = 0; i < 4; ++i) {
        hasher.begin(seed);
        hasher.add(data, strlen(data), false);
        seed = hasher.end();
    }

    EXPECT_NE(0, seed);
}
