// Copyright (c) 2026 The Authors. All rights reserved.
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
// Created: 2026/06/05 00:23

#include "cpp/pl/sstv2/codec/comparable.h"

#include <gtest/gtest.h>

#include <string>

namespace pl::sstv2::codec {
namespace {

TEST(ComparableUint64Test, OrderPreserving) {
    std::string a, b;
    encode_uint64(100, &a);
    encode_uint64(200, &b);
    EXPECT_LT(a, b);

    std::string c;
    encode_uint64(0, &c);
    EXPECT_LT(c, a);

    std::string d;
    encode_uint64(UINT64_MAX, &d);
    EXPECT_GT(d, b);
}

TEST(ComparableInt64Test, OrderPreserving) {
    std::string neg, zero, pos;
    encode_int64(-100, &neg);
    encode_int64(0, &zero);
    encode_int64(100, &pos);
    EXPECT_LT(neg, zero);
    EXPECT_LT(zero, pos);

    std::string min_val, max_val;
    encode_int64(INT64_MIN, &min_val);
    encode_int64(INT64_MAX, &max_val);
    EXPECT_LT(min_val, neg);
    EXPECT_GT(max_val, pos);
}

TEST(ComparableInt32Test, OrderPreserving) {
    std::string neg, zero, pos;
    encode_int32(-1, &neg);
    encode_int32(0, &zero);
    encode_int32(1, &pos);
    EXPECT_LT(neg, zero);
    EXPECT_LT(zero, pos);
}

TEST(ComparableBytesTest, OrderPreserving) {
    std::string a, b, c;
    encode_bytes("abc", &a);
    encode_bytes("abd", &b);
    encode_bytes("abcd", &c);
    EXPECT_LT(a, b);  // "abc" < "abd"
    EXPECT_LT(a, c);  // "abc" < "abcd" (prefix)
}

TEST(ComparableBytesTest, EmptyString) {
    std::string empty, nonempty;
    encode_bytes("", &empty);
    encode_bytes("a", &nonempty);
    EXPECT_LT(empty, nonempty);
}

TEST(ComparableBytesTest, LongString) {
    // Test with strings longer than 8 bytes (multiple groups).
    std::string a, b;
    encode_bytes("12345678X", &a);  // 9 bytes: 2 groups
    encode_bytes("12345678Y", &b);
    EXPECT_LT(a, b);
}

TEST(ComparableDescTest, Uint64Reversed) {
    std::string a, b;
    encode_uint64_desc(100, &a);
    encode_uint64_desc(200, &b);
    EXPECT_GT(a, b); // descending: larger value -> smaller encoding
}

TEST(ComparableDescTest, Int64Reversed) {
    std::string neg, zero, pos;
    encode_int64_desc(-100, &neg);
    encode_int64_desc(0, &zero);
    encode_int64_desc(100, &pos);
    EXPECT_GT(neg, zero);  // descending
    EXPECT_GT(zero, pos);
}

TEST(ComparableDescTest, BytesReversed) {
    std::string a, b;
    encode_bytes_desc("abc", &a);
    encode_bytes_desc("abd", &b);
    EXPECT_GT(a, b); // descending
}

} // namespace
} // namespace pl::sstv2::codec
