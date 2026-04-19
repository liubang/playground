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

#include "cpp/pl/ascii_table/pretty.h"

#include <gtest/gtest.h>
#include <stdexcept>

namespace pl::pretty {
namespace {

TEST(PrettyTest, RendersBorderedTablesByDefault) {
    Pretty table({"ID", "Name"});
    table.add_row({"1", "alice"});

    EXPECT_EQ(
        "+============+\n"
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

    EXPECT_EQ(
        "ID Name \n"
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

    EXPECT_EQ(
        "ID Name \n"
        "1  alice\n"
        "2  bob  \n",
        table.str());
}

} // namespace
} // namespace pl::pretty
