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

#include <gtest/gtest.h>

#include "cpp/pl/flux/syntax/strconv.h"

namespace pl::flux::syntax {
namespace {

TEST(StrConvTest, ParseTextDecodesCommonEscapesAndHex) {
    auto parsed = StrConv::parse_text(R"(line\n\tquote\"slash\\money\$\x41)");

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ("line\n\tquote\"slash\\money$A", *parsed);
}

TEST(StrConvTest, ParseTextRejectsIncompleteHexEscape) {
    auto parsed = StrConv::parse_text(R"(\x4)");

    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("\\x followed by 1 char"), std::string::npos);
}

TEST(StrConvTest, ParseStringRequiresQuotes) {
    auto parsed = StrConv::parse_string("unquoted");

    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("invalid string literal"), std::string::npos);
}

TEST(StrConvTest, ParseRegexUnescapesSlashAndHex) {
    auto parsed = StrConv::parse_regex(R"(/cpu\/total\x2fusage/)");

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ("cpu/total/usage", *parsed);
}

TEST(StrConvTest, ParseTimeAcceptsDateAndRFC3339Zulu) {
    auto date = StrConv::parse_time("2026-04-23");
    auto timestamp = StrConv::parse_time("2026-04-23T12:34:56Z");

    ASSERT_TRUE(date.ok()) << date.status();
    ASSERT_TRUE(timestamp.ok()) << timestamp.status();
    EXPECT_EQ(2026 - 1900, date->tm_year);
    EXPECT_EQ(4 - 1, date->tm_mon);
    EXPECT_EQ(23, date->tm_mday);
    EXPECT_EQ(12, timestamp->tm_hour);
    EXPECT_EQ(34, timestamp->tm_min);
    EXPECT_EQ(56, timestamp->tm_sec);
}

TEST(StrConvTest, ParseDurationNormalizesMicrosecondsAndCompoundsUnits) {
    auto parsed = StrConv::parse_duration("10ms5µs");

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    ASSERT_EQ(2, parsed->size());
    EXPECT_EQ(10, (*parsed)[0]->magnitude);
    EXPECT_EQ("ms", (*parsed)[0]->unit);
    EXPECT_EQ(5, (*parsed)[1]->magnitude);
    EXPECT_EQ("us", (*parsed)[1]->unit);
}

TEST(StrConvTest, ParseUnitRejectsDanglingMicrosecondPrefix) {
    size_t index = 0;
    auto unit = StrConv::parse_unit("µ", index);

    ASSERT_FALSE(unit.ok());
    EXPECT_NE(unit.status().message().find("unterminated microsecond unit"), std::string::npos);
}

TEST(StrConvTest, ParseTimeAcceptsZoneOffsetAndNormalizesToUtc) {
    auto offset = StrConv::parse_time("2024-01-01T08:30:00+08:00");

    ASSERT_TRUE(offset.ok()) << offset.status();
    // 08:30 at +08:00 is 00:30 UTC.
    EXPECT_EQ(2024 - 1900, offset->tm_year);
    EXPECT_EQ(0, offset->tm_mon);
    EXPECT_EQ(1, offset->tm_mday);
    EXPECT_EQ(0, offset->tm_hour);
    EXPECT_EQ(30, offset->tm_min);
    EXPECT_EQ(0, offset->tm_sec);

    auto crossing = StrConv::parse_time("2024-01-01T00:30:00+08:00");

    ASSERT_TRUE(crossing.ok()) << crossing.status();
    // Crosses the day boundary: 2023-12-31T16:30:00Z.
    EXPECT_EQ(2023 - 1900, crossing->tm_year);
    EXPECT_EQ(11, crossing->tm_mon);
    EXPECT_EQ(31, crossing->tm_mday);
    EXPECT_EQ(16, crossing->tm_hour);
    EXPECT_EQ(30, crossing->tm_min);

    auto negative = StrConv::parse_time("2024-01-01T00:30:00-01:00");

    ASSERT_TRUE(negative.ok()) << negative.status();
    EXPECT_EQ(1, negative->tm_hour);
    EXPECT_EQ(30, negative->tm_min);
}

TEST(StrConvTest, ParseTimeAcceptsFractionalSecondsAndMissingOffset) {
    auto fractional = StrConv::parse_time("2024-01-01T00:00:00.500Z");

    ASSERT_TRUE(fractional.ok()) << fractional.status();
    EXPECT_EQ(0, fractional->tm_hour);
    EXPECT_EQ(0, fractional->tm_sec);

    // The scanner accepts a date-time without any zone offset; it is treated
    // as UTC rather than rejected.
    auto naive = StrConv::parse_time("2024-01-01T12:34:56");

    ASSERT_TRUE(naive.ok()) << naive.status();
    EXPECT_EQ(12, naive->tm_hour);
    EXPECT_EQ(34, naive->tm_min);
    EXPECT_EQ(56, naive->tm_sec);
}

TEST(StrConvTest, ParseTimeRejectsGarbage) {
    EXPECT_FALSE(StrConv::parse_time("2024-13-01T00:00:00Z").ok());
    EXPECT_FALSE(StrConv::parse_time("not-a-time").ok());
    EXPECT_FALSE(StrConv::parse_time("2024-01-01T99:00:00Z").ok());
}

TEST(StrConvTest, ParseMagnitudeRejectsOverflow) {
    size_t index = 0;
    auto parsed = StrConv::parse_magnitude("99999999999999999999999999s", index);

    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("overflows int64"), std::string::npos);
}

} // namespace
} // namespace pl::flux::syntax
