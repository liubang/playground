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

#include "cpp/pl/flux/strconv.h"

#include <gtest/gtest.h>

namespace pl {
namespace {

TEST(StrConvTest, ParseTextDecodesCommonEscapesAndHex) {
    auto parsed = StrConv::parse_text(R"(line\n\tquote\"slash\\money\$\x41)");

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ("line\n\tquote\"slash\\money$A", *parsed);
}

TEST(StrConvTest, ParseTextRejectsIncompleteHexEscape) {
    auto parsed = StrConv::parse_text(R"(\x4)");

    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("\\x followed by 1 char"),
              std::string::npos);
}

TEST(StrConvTest, ParseStringRequiresQuotes) {
    auto parsed = StrConv::parse_string("unquoted");

    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("invalid string literal"),
              std::string::npos);
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
    EXPECT_NE(unit.status().message().find("unterminated microsecond unit"),
              std::string::npos);
}

}  // namespace
}  // namespace pl
