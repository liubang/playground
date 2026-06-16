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
// Created: 2026/05/17 21:47

#include <gtest/gtest.h>
#include <sstream>
#include <string>

#include "cpp/pl/ascii_table/pretty.h"

namespace pl::pretty {
namespace {

// =========================================================================
// Pipeline API
// =========================================================================

TEST(PrettyTest, PipelineBasic) {
    auto t = header("ID", "Name") | row("1", "alice");
    t.borders(false);

    EXPECT_EQ("ID Name \n"
              "1  alice\n",
              t.str());
}

TEST(PrettyTest, PipelineWithBorders) {
    auto t = header("ID", "Name") | row("1", "alice");
    t.borders(true);

    EXPECT_EQ("+============+\n"
              "| ID | Name  |\n"
              "+============+\n"
              "| 1  | alice |\n"
              "+============+\n",
              t.str());
}

TEST(PrettyTest, PipelineWithSeparator) {
    auto t = header("ID", "Name") | row("1", "alice") | sep('-') | row("2", "bob");
    t.borders(true);

    EXPECT_EQ("+============+\n"
              "| ID | Name  |\n"
              "+============+\n"
              "| 1  | alice |\n"
              "+------------+\n"
              "| 2  | bob   |\n"
              "+============+\n",
              t.str());
}

TEST(PrettyTest, PipelineWithMultipleSeps) {
    auto t = header("A", "B") | row("1", "2") | sep('~') | row("3", "4") | sep('=') | row("5", "6");
    t.borders(true);

    std::string result = t.str();
    EXPECT_NE(std::string::npos, result.find("~"));
    EXPECT_NE(std::string::npos, result.find("1"));
    EXPECT_NE(std::string::npos, result.find("5"));
}

TEST(PrettyTest, PipelineBordersOffHidesSeps) {
    // Without borders, separator rows are invisible (no line drawn).
    // The "---" row was added as a data row (3 columns), not a sep.
    auto t = header("ID", "Name") | row("1", "alice");
    t.borders(false);
    t = t | sep('-') | row("2", "bob");

    EXPECT_EQ("ID Name \n"
              "1  alice\n"
              "2  bob  \n",
              t.str());
}

// =========================================================================
// Imperative API
// =========================================================================

TEST(PrettyTest, ImperativeBasic) {
    Table t;
    t.header({"ID", "Name"}).data({"1", "alice"}).borders(false);

    EXPECT_EQ("ID Name \n"
              "1  alice\n",
              t.str());
}

TEST(PrettyTest, ImperativeMultipleRows) {
    Table t;
    t.header({"ID", "Name"});
    t.data({"1", "alice"});
    t.data({"2", "bob"});

    EXPECT_NE(std::string::npos, t.str().find("alice"));
    EXPECT_NE(std::string::npos, t.str().find("bob"));
}

// =========================================================================
// Output
// =========================================================================

TEST(PrettyTest, RenderToOstream) {
    auto t = header("A", "B") | row("1", "2");
    t.borders(false);

    std::ostringstream oss;
    t.render(oss);
    EXPECT_FALSE(oss.str().empty());
    EXPECT_EQ(t.str(), oss.str());
}

TEST(PrettyTest, RenderToStdout) {
    auto t = header("X") | row("1");
    t.borders(false);

    testing::internal::CaptureStdout();
    t.render();
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(output.empty());
}

TEST(PrettyTest, OperatorShift) {
    auto t = header("K", "V") | row("foo", "bar");
    t.borders(false);

    std::ostringstream oss;
    oss << t;
    EXPECT_EQ(t.str(), oss.str());
}

// =========================================================================
// Edge cases
// =========================================================================

TEST(PrettyTest, SingleColumn) {
    auto t = header("Status") | row("OK") | row("FAIL");
    t.borders(false);

    std::string result = t.str();
    EXPECT_NE(std::string::npos, result.find("Status"));
    EXPECT_NE(std::string::npos, result.find("OK"));
    EXPECT_NE(std::string::npos, result.find("FAIL"));
}

TEST(PrettyTest, HeaderOnly) {
    // header() alone returns a Header tag — use the imperative API
    // for a table that only has a header row.
    Table t;
    t.header({"Col1", "Col2", "Col3"});
    t.borders(false);

    std::string result = t.str();
    EXPECT_NE(std::string::npos, result.find("Col1"));
    EXPECT_NE(std::string::npos, result.find("Col2"));
    EXPECT_NE(std::string::npos, result.find("Col3"));
}

TEST(PrettyTest, EmptyCells) {
    auto t = header("A", "B") | row("", "") | row("x", "");
    t.borders(false);

    std::string result = t.str();
    EXPECT_FALSE(result.empty());
    EXPECT_NE(std::string::npos, result.find('x'));
}

TEST(PrettyTest, ColumnAutoExpand) {
    // clang-format off
    auto t = header("Key")                   // 1 col
           | row("animal", "cat")            // 2 cols
           | row("fruit", "apple", "red");   // 3 cols
    // clang-format on
    t.borders(false);

    std::string result = t.str();
    EXPECT_NE(std::string::npos, result.find("animal"));
    EXPECT_NE(std::string::npos, result.find("apple"));
    EXPECT_NE(std::string::npos, result.find("red"));
}

TEST(PrettyTest, CJKCharacters) {
    auto t = header("姓名", "年龄") | row("张三", "25") | row("李四十", "30");
    t.borders(false);

    std::string result = t.str();
    EXPECT_NE(std::string::npos, result.find("张三"));
    EXPECT_NE(std::string::npos, result.find("李四十"));
}

TEST(PrettyTest, BordersSetterReturnsSelf) {
    Table t;
    auto& ref = t.borders(false);
    EXPECT_EQ(&t, &ref);
}

TEST(PrettyTest, PipelineCopiesLvalueTable) {
    auto t1 = header("A") | row("1");
    t1.borders(false);
    auto t2 = t1 | row("2"); // copies t1

    // t1 should still have 2 rows (header + "1")
    EXPECT_NE(std::string::npos, t1.str().find('1'));
    // t2 should have 3 rows (header + "1" + "2")
    EXPECT_NE(std::string::npos, t2.str().find('2'));
}

} // namespace
} // namespace pl::pretty
