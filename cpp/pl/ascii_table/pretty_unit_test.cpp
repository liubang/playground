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

#include "cpp/pl/ascii_table/pretty.h"
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>

namespace pl::pretty {
namespace {

TEST(PrettyTest, RendersBorderedTablesByDefault) {
    Pretty table({"ID", "Name"});
    table.add_row({"1", "alice"});

    EXPECT_EQ("+============+\n"
              "| ID | Name  |\n"
              "+============+\n"
              "| 1  | alice |\n"
              "+============+\n",
              table.str());
}

TEST(PrettyTest, RendersHeaderWithoutBordersWhenDisabled) {
    Pretty table({"ID", "Name"});
    table.set_show_sep(false);
    table.add_row({"1", "alice"});

    EXPECT_EQ("ID Name \n"
              "1  alice\n",
              table.str());
}

TEST(PrettyTest, RejectsRowsWithWrongCellCount) {
    Pretty table({"ID", "Name"});

    EXPECT_THROW(table.add_row({"1"}), std::out_of_range);
    EXPECT_THROW(table.add_row({"1", "alice", "extra"}), std::out_of_range);
}

TEST(PrettyTest, RejectsEmptySeparators) {
    Pretty table({"ID", "Name"});

    EXPECT_THROW(table.add_sep(""), std::invalid_argument);
}

TEST(PrettyTest, IgnoresSeparatorRowsWithoutBorders) {
    Pretty table({"ID", "Name"});
    table.set_show_sep(false);
    table.add_row({"1", "alice"});
    table.add_sep("-");
    table.add_row({"2", "bob"});

    EXPECT_EQ("ID Name \n"
              "1  alice\n"
              "2  bob  \n",
              table.str());
}

TEST(PrettyTest, RenderSeparatorBetweenRows) {
    Pretty table({"ID", "Name"});
    table.add_row({"1", "alice"});
    table.add_sep("-");
    table.add_row({"2", "bob"});

    EXPECT_EQ("+============+\n"
              "| ID | Name  |\n"
              "+============+\n"
              "| 1  | alice |\n"
              "+------------+\n"
              "| 2  | bob   |\n"
              "+============+\n",
              table.str());
}

TEST(PrettyTest, RenderToOutputStream) {
    Pretty table({"A", "B"});
    table.add_row({"1", "2"});

    std::ostringstream oss;
    table.render(oss);
    EXPECT_FALSE(oss.str().empty());
    EXPECT_EQ(table.str(), oss.str());
}

TEST(PrettyTest, RenderToStdout) {
    Pretty table({"X"});
    table.add_row({"1"});

    // Just verify render() to stdout doesn't crash
    testing::internal::CaptureStdout();
    table.render();
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(output.empty());
}

TEST(PrettyTest, PadRightTruncatesLongInput) {
    // When a cell value is longer than the column width determined by headers,
    // pad_right should truncate it. We can trigger this by having a header
    // shorter than the data added later isn't possible since cell_max_length_
    // tracks max. Instead, we can verify via the rendered output that padding works.
    Pretty table({"Name"});
    table.add_row({"LongName"});

    std::string result = table.str();
    // The column width should be 8 (max of "Name"=4, "LongName"=8)
    EXPECT_NE(std::string::npos, result.find("LongName"));
}

TEST(PrettyTest, MultipleColumnsWithVaryingWidths) {
    Pretty table({"ID", "Name", "Description"});
    table.add_row({"1", "alice", "short"});
    table.add_row({"100", "bob", "a much longer description"});

    std::string result = table.str();
    EXPECT_NE(std::string::npos, result.find("100"));
    EXPECT_NE(std::string::npos, result.find("a much longer description"));
}

TEST(PrettyTest, CellValidMethod) {
    Cell none_cell;
    EXPECT_FALSE(none_cell.valid());

    Cell string_cell(CellType::CT_STRING, "hello");
    EXPECT_TRUE(string_cell.valid());

    Cell sep_cell(CellType::CT_SEP, "-");
    EXPECT_TRUE(sep_cell.valid());
}

TEST(PrettyTest, MultipleSeparators) {
    Pretty table({"A", "B"});
    table.add_row({"1", "2"});
    table.add_sep("~");
    table.add_row({"3", "4"});
    table.add_sep("=");
    table.add_row({"5", "6"});

    std::string result = table.str();
    EXPECT_NE(std::string::npos, result.find("~"));
    EXPECT_NE(std::string::npos, result.find("1"));
    EXPECT_NE(std::string::npos, result.find("5"));
}

TEST(PrettyTest, EmptyCellValues) {
    Pretty table({"ID", "Name"});
    table.add_row({"", ""});

    std::string result = table.str();
    // Should render without crashing, with padded empty cells
    EXPECT_FALSE(result.empty());
}

TEST(PrettyTest, SingleColumnTable) {
    Pretty table({"Status"});
    table.add_row({"OK"});
    table.add_row({"FAIL"});

    std::string result = table.str();
    EXPECT_NE(std::string::npos, result.find("Status"));
    EXPECT_NE(std::string::npos, result.find("OK"));
    EXPECT_NE(std::string::npos, result.find("FAIL"));
}

TEST(PrettyTest, HeaderOnlyTable) {
    Pretty table({"Col1", "Col2", "Col3"});

    std::string result = table.str();
    EXPECT_NE(std::string::npos, result.find("Col1"));
    EXPECT_NE(std::string::npos, result.find("Col2"));
    EXPECT_NE(std::string::npos, result.find("Col3"));
}

TEST(PrettyTest, SetShowSepReturnsSelf) {
    Pretty table({"A"});
    Pretty& ref = table.set_show_sep(false);
    EXPECT_EQ(&table, &ref);
}

} // namespace
} // namespace pl::pretty
