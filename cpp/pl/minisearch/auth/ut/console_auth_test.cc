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
// Created: 2026/08/17

#include <cstdio>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "cpp/pl/minisearch/auth/console_auth.h"

namespace pl::minisearch::auth {
namespace {

class ConsoleAuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string tmpl = "/tmp/minisearch_console_auth_test_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        ASSERT_NE(nullptr, ::mkdtemp(buf.data()));
        tmp_dir_ = buf.data();
        store_path_ = tmp_dir_ + "/console_users.json";
    }

    void TearDown() override {
        if (!tmp_dir_.empty()) {
            std::string cmd = "rm -rf '" + tmp_dir_ + "'";
            EXPECT_EQ(0, system(cmd.c_str()));
        }
    }

    std::string tmp_dir_;
    std::string store_path_;
};

TEST_F(ConsoleAuthTest, BootstrapCreatesAdmin) {
    ConsoleAuth::Options opts;
    opts.path = store_path_;
    opts.bootstrap_user = "admin";
    opts.bootstrap_password = "secret";
    ConsoleAuth auth(std::move(opts));

    EXPECT_TRUE(auth.enabled());
    auto token = auth.Login("admin", "secret");
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(token->substr(0, 4), "mss_");

    auto principal = auth.Authenticate(*token);
    ASSERT_TRUE(principal.has_value());
    EXPECT_EQ(principal->tenant, "default");
    EXPECT_EQ(principal->role, Role::kAdmin);
}

TEST_F(ConsoleAuthTest, WrongPasswordFails) {
    ConsoleAuth::Options opts;
    opts.path = store_path_;
    opts.bootstrap_user = "admin";
    opts.bootstrap_password = "secret";
    ConsoleAuth auth(std::move(opts));

    auto token = auth.Login("admin", "wrong");
    EXPECT_FALSE(token.has_value());
}

TEST_F(ConsoleAuthTest, LogoutInvalidatesSession) {
    ConsoleAuth::Options opts;
    opts.path = store_path_;
    opts.bootstrap_user = "admin";
    opts.bootstrap_password = "secret";
    ConsoleAuth auth(std::move(opts));

    auto token = auth.Login("admin", "secret");
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(auth.Authenticate(*token).has_value());

    auth.Logout(*token);
    EXPECT_FALSE(auth.Authenticate(*token).has_value());
}

TEST_F(ConsoleAuthTest, ChangePassword) {
    ConsoleAuth::Options opts;
    opts.path = store_path_;
    opts.bootstrap_user = "admin";
    opts.bootstrap_password = "secret";
    ConsoleAuth auth(std::move(opts));

    EXPECT_FALSE(auth.ChangePassword("admin", "wrong", "newpass"));
    EXPECT_TRUE(auth.ChangePassword("admin", "secret", "newpassword"));

    // old password no longer works
    EXPECT_FALSE(auth.Login("admin", "secret").has_value());
    // new password works
    EXPECT_TRUE(auth.Login("admin", "newpassword").has_value());
}

TEST_F(ConsoleAuthTest, ChangePasswordTooShort) {
    ConsoleAuth::Options opts;
    opts.path = store_path_;
    opts.bootstrap_user = "admin";
    opts.bootstrap_password = "secret";
    ConsoleAuth auth(std::move(opts));

    EXPECT_FALSE(auth.ChangePassword("admin", "secret", "short"));
}

TEST_F(ConsoleAuthTest, PersistAcrossInstances) {
    {
        ConsoleAuth::Options opts;
        opts.path = store_path_;
        opts.bootstrap_user = "admin";
        opts.bootstrap_password = "secret";
        ConsoleAuth auth(std::move(opts));
        ASSERT_TRUE(auth.ChangePassword("admin", "secret", "newpassword"));
    }
    // New instance should load the persisted store (password already changed).
    // Bootstrap should NOT overwrite existing users.
    ConsoleAuth::Options opts;
    opts.path = store_path_;
    opts.bootstrap_user = "admin";
    opts.bootstrap_password = "secret";
    ConsoleAuth auth(std::move(opts));

    EXPECT_FALSE(auth.Login("admin", "secret").has_value());
    EXPECT_TRUE(auth.Login("admin", "newpassword").has_value());
}

TEST_F(ConsoleAuthTest, NoBootstrapMeansDisabled) {
    ConsoleAuth::Options opts;
    opts.path = store_path_;
    // no bootstrap user/password
    ConsoleAuth auth(std::move(opts));
    EXPECT_FALSE(auth.enabled());
}

TEST_F(ConsoleAuthTest, InMemoryMode) {
    ConsoleAuth::Options opts;
    opts.path = ""; // pure in-memory
    opts.bootstrap_user = "admin";
    opts.bootstrap_password = "secret";
    ConsoleAuth auth(std::move(opts));

    EXPECT_TRUE(auth.enabled());
    auto token = auth.Login("admin", "secret");
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(auth.Authenticate(*token).has_value());
}

} // namespace
} // namespace pl::minisearch::auth
