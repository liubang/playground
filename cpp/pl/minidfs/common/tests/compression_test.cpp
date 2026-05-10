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

#include "cpp/pl/minidfs/common/compression.h"

#include <gtest/gtest.h>

namespace pl::minidfs {
namespace {

// ============================================================================
// CompressionType enum values
// ============================================================================

TEST(CompressionTest, EnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(CompressionType::kNone), 0u);
    EXPECT_EQ(static_cast<uint8_t>(CompressionType::kSnappy), 1u);
    EXPECT_EQ(static_cast<uint8_t>(CompressionType::kZstd), 2u);
}

TEST(CompressionTest, EnumUnderlyingType) {
    static_assert(std::is_same_v<std::underlying_type_t<CompressionType>, uint8_t>);
}

// ============================================================================
// compression_type_name tests
// ============================================================================

TEST(CompressionTest, NameNone) {
    EXPECT_EQ(compression_type_name(CompressionType::kNone), "none");
}

TEST(CompressionTest, NameSnappy) {
    EXPECT_EQ(compression_type_name(CompressionType::kSnappy), "snappy");
}

TEST(CompressionTest, NameZstd) {
    EXPECT_EQ(compression_type_name(CompressionType::kZstd), "zstd");
}

TEST(CompressionTest, NameIsConstexpr) {
    // Verify the function is usable in constexpr context
    constexpr auto name = compression_type_name(CompressionType::kNone);
    EXPECT_EQ(name, "none");
}

TEST(CompressionTest, NameReturnsStringView) {
    auto name = compression_type_name(CompressionType::kSnappy);
    static_assert(std::is_same_v<decltype(name), std::string_view>);
    EXPECT_FALSE(name.empty());
}

} // namespace
} // namespace pl::minidfs
