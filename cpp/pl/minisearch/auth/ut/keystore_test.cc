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

// KeyStore semantics (DESIGN.md §10.3): issue/authenticate/revoke,
// persistence round-trip, one-shot bootstrap.

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

#include "cpp/pl/minisearch/auth/keystore.h"

namespace pma = pl::minisearch::auth;

namespace {

std::string temp_dir() {
    std::string tmpl = "/tmp/minisearch_keystore_test_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = ::mkdtemp(buf.data());
    return dir == nullptr ? "" : std::string(dir);
}

} // namespace

TEST(KeyStoreTest, IssueAuthenticateRevoke) {
    const std::string dir = temp_dir();
    ASSERT_FALSE(dir.empty());
    pma::KeyStore store(dir + "/auth/keys.json");

    auto issued = store.Issue("team-a", pma::Role::kWriter, {"kb"});
    ASSERT_FALSE(issued.key.empty());
    ASSERT_EQ(issued.key.substr(0, 4), "msk_");

    auto principal = store.Authenticate(issued.key);
    ASSERT_TRUE(principal.has_value());
    EXPECT_EQ(principal->tenant, "team-a");
    EXPECT_EQ(principal->role, pma::Role::kWriter);
    ASSERT_EQ(principal->collections.size(), 1u);
    EXPECT_EQ(principal->collections[0], "kb");

    EXPECT_FALSE(store.Authenticate("msk_bogus").has_value());

    ASSERT_TRUE(store.Revoke(issued.key_id));
    EXPECT_FALSE(store.Authenticate(issued.key).has_value()); // immediate
    EXPECT_FALSE(store.Revoke(issued.key_id));
    std::string cmd = "rm -rf '" + dir + "'";
    ASSERT_EQ(0, system(cmd.c_str()));
}

TEST(KeyStoreTest, PersistenceRoundTrip) {
    const std::string dir = temp_dir();
    ASSERT_FALSE(dir.empty());
    const std::string path = dir + "/auth/keys.json";
    ::mkdir((dir + "/auth").c_str(), 0755);

    pma::KeyStore::Issued first;
    pma::KeyStore::Issued second;
    {
        pma::KeyStore store(path);
        first = store.Issue("t1", pma::Role::kAdmin, {});
        second = store.Issue("t1", pma::Role::kReader, {"c1", "c2"});
        ASSERT_TRUE(store.Revoke(first.key_id));
    }
    {
        pma::KeyStore store(path);
        EXPECT_FALSE(store.Authenticate(first.key).has_value()); // revoked stays revoked
        auto principal = store.Authenticate(second.key);
        ASSERT_TRUE(principal.has_value());
        EXPECT_EQ(principal->role, pma::Role::kReader);
        ASSERT_EQ(principal->collections.size(), 2u);
        // key ids continue without reuse
        auto next = store.Issue("t1", pma::Role::kWriter, {});
        EXPECT_NE(next.key_id, first.key_id);
        EXPECT_NE(next.key_id, second.key_id);
    }
    std::string cmd = "rm -rf '" + dir + "'";
    ASSERT_EQ(0, system(cmd.c_str()));
}

TEST(KeyStoreTest, BootstrapIsOneShot) {
    const std::string dir = temp_dir();
    ASSERT_FALSE(dir.empty());
    pma::KeyStore store(dir + "/auth/keys.json");

    auto boot = store.BootstrapIfNeeded(dir);
    ASSERT_TRUE(boot.has_value());
    ASSERT_TRUE(store.Authenticate(boot->key).has_value());
    EXPECT_EQ(store.Authenticate(boot->key)->role, pma::Role::kAdmin);
    EXPECT_EQ(store.Authenticate(boot->key)->tenant, "default");

    // bootstrap.key file holds the plaintext exactly once
    std::ifstream in(dir + "/bootstrap.key");
    ASSERT_TRUE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, boot->key + "\n");

    EXPECT_FALSE(store.BootstrapIfNeeded(dir).has_value()); // non-empty store
    std::string cmd = "rm -rf '" + dir + "'";
    ASSERT_EQ(0, system(cmd.c_str()));
}

TEST(KeyStoreTest, RoleStrings) {
    EXPECT_EQ(pma::RoleToString(pma::Role::kTenantAdmin), "tenant_admin");
    EXPECT_EQ(pma::RoleFromString("writer"), pma::Role::kWriter);
    EXPECT_FALSE(pma::RoleFromString("root").has_value());
}

TEST(KeyStoreTest, InvalidResourceNamesRejected) {
    pma::KeyStore store("");
    // 空格、斜杠、点号、CJK 都不是合法资源名
    EXPECT_TRUE(store.Issue("bad tenant", pma::Role::kReader, {}).key.empty());
    EXPECT_TRUE(store.Issue("bad/name", pma::Role::kReader, {}).key.empty());
    EXPECT_TRUE(store.Issue("bad.name", pma::Role::kReader, {}).key.empty());
    EXPECT_TRUE(store.Issue("租户", pma::Role::kReader, {}).key.empty());
    // 合法：横杠/下划线/数字
    EXPECT_FALSE(store.Issue("team-a_01", pma::Role::kReader, {}).key.empty());
    // collection 白名单里的非法名同样拒绝
    EXPECT_TRUE(store.Issue("ok", pma::Role::kReader, {"bad/name"}).key.empty());
}

TEST(KeyStoreTest, MetadataTracked) {
    pma::KeyStore store("");
    auto issued = store.Issue("t1", pma::Role::kWriter, {});
    ASSERT_FALSE(issued.key.empty());
    auto entries = store.List();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_GT(entries[0].created_at, 0);
    EXPECT_FALSE(entries[0].revoked);
    ASSERT_TRUE(store.Revoke(issued.key_id));
    entries = store.List();
    ASSERT_EQ(entries.size(), 1u); // 软删除：条目仍在
    EXPECT_TRUE(entries[0].revoked);
}
