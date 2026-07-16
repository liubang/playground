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
// Created: 2026/05/10 21:00

#include <gtest/gtest.h>

#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/namenode/namespace_manager.h"
#include "cpp/pl/minidfs/namenode/tests/mock_metadata_store.h"

namespace pl::minidfs {
namespace {

class NamespaceManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<testing::MockMetadataStore>();
        mgr_ = std::make_unique<NamespaceManager>(store_.get());
    }

    std::unique_ptr<testing::MockMetadataStore> store_;
    std::unique_ptr<NamespaceManager> mgr_;
};

// mkdir tests
TEST_F(NamespaceManagerTest, MkdirSimple) {
    auto result = mgr_->mkdir("/foo", "user1", "grp1", 0755);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().name, "foo");
    EXPECT_EQ(result.value().type, InodeType::kDirectory);
    EXPECT_EQ(result.value().parent_id, 1); // root
    EXPECT_EQ(result.value().owner, "user1");
}

TEST_F(NamespaceManagerTest, MkdirNested) {
    // Create /a
    auto a = mgr_->mkdir("/a", "user1", "grp1", 0755);
    ASSERT_TRUE(a.hasValue());

    // Create /a/b without create_parent — parent exists.
    auto b = mgr_->mkdir("/a/b", "user1", "grp1", 0755);
    ASSERT_TRUE(b.hasValue());
    EXPECT_EQ(b.value().name, "b");
    EXPECT_EQ(b.value().parent_id, a.value().inode_id);
}

TEST_F(NamespaceManagerTest, MkdirNestedParentMissing) {
    // Create /a/b/c without create_parent — intermediate /a and /a/b don't exist.
    auto result = mgr_->mkdir("/a/b/c", "user1", "grp1", 0755, /*create_parent=*/false);
    ASSERT_TRUE(result.hasError());
}

TEST_F(NamespaceManagerTest, MkdirWithCreateParent) {
    auto result = mgr_->mkdir("/a/b/c", "user1", "grp1", 0755, /*create_parent=*/true);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().name, "c");

    // Verify intermediate dirs were created.
    auto a = mgr_->resolve_path("/a");
    ASSERT_TRUE(a.hasValue());
    auto b = mgr_->resolve_path("/a/b");
    ASSERT_TRUE(b.hasValue());
}

TEST_F(NamespaceManagerTest, MkdirIdempotent) {
    auto first = mgr_->mkdir("/foo", "user1", "grp1", 0755);
    ASSERT_TRUE(first.hasValue());

    // Second mkdir should succeed (idempotent for directories).
    auto second = mgr_->mkdir("/foo", "user1", "grp1", 0755);
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value().inode_id, second.value().inode_id);
}

TEST_F(NamespaceManagerTest, MkdirRootIsNoop) {
    auto result = mgr_->mkdir("/", "user1", "grp1", 0755);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().inode_id, 1);
}

TEST_F(NamespaceManagerTest, MkdirInvalidPath) {
    auto result = mgr_->mkdir("relative/path", "user1", "grp1", 0755);
    ASSERT_TRUE(result.hasError());
}

// create_file tests
TEST_F(NamespaceManagerTest, CreateFileSimple) {
    auto result = mgr_->create_file("/myfile.txt", "user1", "grp1", 0644, 3, 128 * 1024 * 1024);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().name, "myfile.txt");
    EXPECT_EQ(result.value().type, InodeType::kFile);
    EXPECT_EQ(result.value().state, FileState::kUnderConstruction);
    EXPECT_EQ(result.value().replication, 3u);
}

TEST_F(NamespaceManagerTest, CreateFileParentMissing) {
    auto result = mgr_->create_file("/no/such/dir/file.txt", "user1", "grp1", 0644, 3, 128 * kMB);
    ASSERT_TRUE(result.hasError());
}

TEST_F(NamespaceManagerTest, CreateFileDuplicate) {
    mgr_->create_file("/dup.txt", "user1", "grp1", 0644, 3, 128 * kMB);
    auto result = mgr_->create_file("/dup.txt", "user1", "grp1", 0644, 3, 128 * kMB);
    ASSERT_TRUE(result.hasError());
}

// remove tests
TEST_F(NamespaceManagerTest, RemoveFile) {
    mgr_->create_file("/todel.txt", "user1", "grp1", 0644, 3, 128 * kMB);
    auto result = mgr_->remove("/todel.txt");
    ASSERT_TRUE(result.hasValue());

    // Verify it's gone.
    auto stat = mgr_->get_file_status("/todel.txt");
    ASSERT_TRUE(stat.hasError());
}

TEST_F(NamespaceManagerTest, RemoveEmptyDir) {
    mgr_->mkdir("/empty", "user1", "grp1", 0755);
    auto result = mgr_->remove("/empty");
    ASSERT_TRUE(result.hasValue());
}

TEST_F(NamespaceManagerTest, RemoveNonEmptyDirFails) {
    mgr_->mkdir("/parent", "user1", "grp1", 0755);
    mgr_->mkdir("/parent/child", "user1", "grp1", 0755);
    auto result = mgr_->remove("/parent", /*recursive=*/false);
    ASSERT_TRUE(result.hasError());
}

TEST_F(NamespaceManagerTest, RemoveRecursive) {
    mgr_->mkdir("/parent", "user1", "grp1", 0755);
    mgr_->mkdir("/parent/child", "user1", "grp1", 0755);
    auto result = mgr_->remove("/parent", /*recursive=*/true);
    ASSERT_TRUE(result.hasValue());

    auto stat = mgr_->get_file_status("/parent");
    ASSERT_TRUE(stat.hasError());
}

TEST_F(NamespaceManagerTest, RemoveRoot) {
    auto result = mgr_->remove("/");
    ASSERT_TRUE(result.hasError());
}

// rename tests
TEST_F(NamespaceManagerTest, RenameFile) {
    mgr_->create_file("/src.txt", "user1", "grp1", 0644, 3, 128 * kMB);
    auto result = mgr_->rename("/src.txt", "/dst.txt");
    ASSERT_TRUE(result.hasValue());

    auto old_stat = mgr_->get_file_status("/src.txt");
    EXPECT_TRUE(old_stat.hasError());

    auto new_stat = mgr_->get_file_status("/dst.txt");
    ASSERT_TRUE(new_stat.hasValue());
    EXPECT_EQ(new_stat.value().path, "/dst.txt");
}

TEST_F(NamespaceManagerTest, RenameToExistingFails) {
    mgr_->create_file("/a.txt", "user1", "grp1", 0644, 3, 128 * kMB);
    mgr_->create_file("/b.txt", "user1", "grp1", 0644, 3, 128 * kMB);
    auto result = mgr_->rename("/a.txt", "/b.txt");
    ASSERT_TRUE(result.hasError());
}

TEST_F(NamespaceManagerTest, RenameSrcNotFound) {
    auto result = mgr_->rename("/nonexist.txt", "/dst.txt");
    ASSERT_TRUE(result.hasError());
}

TEST_F(NamespaceManagerTest, RenameDirectoryToSelfFails) {
    ASSERT_TRUE(mgr_->mkdir("/dir", "user1", "grp1", 0755).hasValue());

    auto result = mgr_->rename("/dir", "/dir");
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument));
}

TEST_F(NamespaceManagerTest, RenameDirectoryToDescendantFails) {
    ASSERT_TRUE(mgr_->mkdir("/src/child", "user1", "grp1", 0755, true).hasValue());

    auto result = mgr_->rename("/src", "/src/child/new-src");
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument));

    auto still_exists = mgr_->get_file_status("/src");
    ASSERT_TRUE(still_exists.hasValue());
    EXPECT_TRUE(still_exists.value().is_dir);
}

// get_file_status / list_status tests
TEST_F(NamespaceManagerTest, GetFileStatus) {
    mgr_->create_file("/status.txt", "owner1", "grp1", 0644, 2, 64 * kMB);
    auto result = mgr_->get_file_status("/status.txt");
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().path, "/status.txt");
    EXPECT_FALSE(result.value().is_dir);
    EXPECT_EQ(result.value().owner, "owner1");
    EXPECT_EQ(result.value().replication, 2u);
}

TEST_F(NamespaceManagerTest, GetFileStatusRoot) {
    auto result = mgr_->get_file_status("/");
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().is_dir);
}

TEST_F(NamespaceManagerTest, ListStatus) {
    mgr_->mkdir("/dir", "user1", "grp1", 0755);
    mgr_->create_file("/file1.txt", "user1", "grp1", 0644, 3, 128 * kMB);

    auto result = mgr_->list_status("/");
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().size(), 2u);
}

TEST_F(NamespaceManagerTest, ListStatusOnFile) {
    mgr_->create_file("/f.txt", "user1", "grp1", 0644, 3, 128 * kMB);
    auto result = mgr_->list_status("/f.txt");
    ASSERT_TRUE(result.hasError());
}

// resolve_path tests
TEST_F(NamespaceManagerTest, ResolvePathRoot) {
    auto result = mgr_->resolve_path("/");
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().inode_id, 1u);
}

TEST_F(NamespaceManagerTest, ResolvePathNotFound) {
    auto result = mgr_->resolve_path("/no/such/path");
    ASSERT_TRUE(result.hasError());
}

// complete_file tests
TEST_F(NamespaceManagerTest, CompleteFile) {
    auto file = mgr_->create_file("/wip.txt", "user1", "grp1", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file.hasValue());
    EXPECT_EQ(file.value().state, FileState::kUnderConstruction);

    auto result = mgr_->complete_file(file.value().inode_id, 1024);
    ASSERT_TRUE(result.hasValue());

    auto stat = mgr_->get_file_status("/wip.txt");
    ASSERT_TRUE(stat.hasValue());
    EXPECT_EQ(stat.value().length, 1024u);
}

TEST_F(NamespaceManagerTest, CompleteFileOnDir) {
    auto result = mgr_->complete_file(1 /* root inode */, 0);
    ASSERT_TRUE(result.hasError());
}

TEST_F(NamespaceManagerTest, SetFileLengthAndReplicationOnClosedFile) {
    auto file = mgr_->create_file("/closed", "user", "group", 0644, 3, 128);
    ASSERT_TRUE(file.hasValue());
    ASSERT_TRUE(mgr_->complete_file(file.value().inode_id, 64).hasValue());

    ASSERT_TRUE(mgr_->set_file_length(file.value().inode_id, 32).hasValue());
    ASSERT_TRUE(mgr_->set_replication(file.value().inode_id, 2).hasValue());

    auto inode = store_->get_inode(file.value().inode_id);
    ASSERT_TRUE(inode.hasValue());
    EXPECT_EQ(inode.value().length, 32u);
    EXPECT_EQ(inode.value().replication, 2u);
}

TEST_F(NamespaceManagerTest, SetFileLengthRejectsUnderConstructionFile) {
    auto file = mgr_->create_file("/wip-length", "user", "group", 0644, 3, 128);
    ASSERT_TRUE(file.hasValue());
    EXPECT_TRUE(mgr_->set_file_length(file.value().inode_id, 0).hasError());
}

TEST_F(NamespaceManagerTest, BeginAppendReopensCompletedFile) {
    auto file = mgr_->create_file("/append", "user", "group", 0644, 3, 128);
    ASSERT_TRUE(file.hasValue());
    ASSERT_TRUE(mgr_->complete_file(file.value().inode_id, 64).hasValue());

    auto reopened = mgr_->begin_append("/append");
    ASSERT_TRUE(reopened.hasValue());
    EXPECT_EQ(reopened.value().state, FileState::kUnderConstruction);
    EXPECT_EQ(reopened.value().length, 64u);
}

TEST_F(NamespaceManagerTest, BeginAppendRejectsUnderConstructionFile) {
    ASSERT_TRUE(mgr_->create_file("/append", "user", "group", 0644, 3, 128).hasValue());
    EXPECT_TRUE(mgr_->begin_append("/append").hasError());
}

} // namespace
} // namespace pl::minidfs
