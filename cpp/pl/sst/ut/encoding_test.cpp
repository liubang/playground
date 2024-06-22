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

#include "cpp/pl/sst/encoding.h"

#include <gtest/gtest.h>
#include <string_view>

TEST(encoding, putAndGetInt) {
    std::string dst;
    int32_t a = 12345;
    pl::encodeInt(&dst, a);
    std::string_view binary(dst);

    auto aa = pl::decodeInt<uint32_t>(binary.data());
    EXPECT_EQ(a, aa);

    uint64_t b = 123456789;
    dst.clear();
    pl::encodeInt(&dst, b);
    binary = dst;

    auto bb = pl::decodeInt<uint64_t>(binary.data());
    EXPECT_EQ(b, bb);

    uint8_t c = 2;
    dst.clear();
    pl::encodeInt(&dst, c);
    binary = dst;

    auto cc = pl::decodeInt<uint8_t>(binary.data());
    EXPECT_EQ(c, cc);
}
