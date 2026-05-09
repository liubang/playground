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
// Created: 2026/05/09

#include "cpp/pl/flux/connector/mysql_source.h"
#include "gtest/gtest.h"

namespace pl::flux::connector {
namespace {

TEST(MySQLSourceTest, ParsesMysqlUrlDsn) {
    auto config_or = ParseMySQLDsn("mysql://flux:flux@192.168.50.31:3307/testdb");

    ASSERT_TRUE(config_or.ok()) << config_or.status();
    EXPECT_EQ("192.168.50.31", config_or->host);
    EXPECT_EQ(3307, config_or->port);
    EXPECT_EQ("flux", config_or->user);
    EXPECT_EQ("flux", config_or->password);
    EXPECT_EQ("testdb", config_or->database);
}

TEST(MySQLSourceTest, ParsesTcpDsnWithDefaultPort) {
    auto config_or = ParseMySQLDsn("flux:secret@tcp(localhost)/testdb");

    ASSERT_TRUE(config_or.ok()) << config_or.status();
    EXPECT_EQ("localhost", config_or->host);
    EXPECT_EQ(3306, config_or->port);
    EXPECT_EQ("flux", config_or->user);
    EXPECT_EQ("secret", config_or->password);
    EXPECT_EQ("testdb", config_or->database);
}

TEST(MySQLSourceTest, RejectsInvalidDsnPort) {
    auto config_or = ParseMySQLDsn("flux:secret@tcp(localhost:70000)/testdb");

    ASSERT_FALSE(config_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, config_or.status().code());
}

} // namespace
} // namespace pl::flux::connector
