// Copyright (c) 2023 The Authors. All rights reserved.
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
// Created: 2023/05/29 17:22

#include "cpp/pl/fs/posix_fs.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <sys/stat.h>
#include <utime.h>

namespace {

class PosixFileSystemTest : public ::testing::Test {
protected:
    void SetUp() override { fs_ = std::make_unique<pl::PosixFileSystem>(); }

    void TearDown() override {
        // Clean up test files
        std::remove("/tmp/pl_fs_test.file");
        std::remove("/tmp/pl_fs_test_renamed.file");
        std::remove("/tmp/pl_fs_test_dir");
        std::remove("/tmp/pl_fs_test_utime.file");
    }

    std::unique_ptr<pl::PosixFileSystem> fs_;
};

TEST_F(PosixFileSystemTest, OpenWriteAndRead) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_APPEND | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    ASSERT_NE(nullptr, fd);

    st = fs_->append(fd, 0, "hello world");
    ASSERT_FALSE(st.hasError());

    st = fs_->fsync(fd, 0);
    ASSERT_FALSE(st.hasError());

    // Check size via fd
    uint64_t size = 0;
    st = fs_->size(fd, &size);
    ASSERT_FALSE(st.hasError());
    EXPECT_EQ(11, size);

    // Drop write fd (let destructor handle close)
    fd.reset();

    // Reopen for read
    pl::FileDescriptorRef read_fd;
    st = fs_->open("/tmp/pl_fs_test.file", O_RDONLY, &read_fd);
    ASSERT_FALSE(st.hasError());

    char buffer[11];
    std::string_view result;
    st = fs_->pread(read_fd, 0, 11, buffer, &result);
    ASSERT_FALSE(st.hasError());
    EXPECT_EQ("hello world", result);
}

TEST_F(PosixFileSystemTest, SizeByPath) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());

    st = fs_->append(fd, 0, "test content");
    ASSERT_FALSE(st.hasError());
    st = fs_->fsync(fd, 0);
    ASSERT_FALSE(st.hasError());
    fd.reset();

    uint64_t size = 0;
    st = fs_->size("/tmp/pl_fs_test.file", &size);
    ASSERT_FALSE(st.hasError());
    EXPECT_EQ(12, size);
}

TEST_F(PosixFileSystemTest, SizeByPathNotExist) {
    uint64_t size = 0;
    auto st = fs_->size("/tmp/pl_fs_nonexistent_file_xyz", &size);
    EXPECT_TRUE(st.hasError());
}

TEST_F(PosixFileSystemTest, SizeByFdAfterWrite) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());

    st = fs_->append(fd, 0, "abcdef");
    ASSERT_FALSE(st.hasError());
    st = fs_->fsync(fd, 0);
    ASSERT_FALSE(st.hasError());

    uint64_t size = 0;
    st = fs_->size(fd, &size);
    ASSERT_FALSE(st.hasError());
    EXPECT_EQ(6, size);
}

TEST_F(PosixFileSystemTest, ExistFile) {
    // Create a file first
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    fd.reset();

    bool exist = false;
    st = fs_->exist("/tmp/pl_fs_test.file", &exist);
    ASSERT_FALSE(st.hasError());
    EXPECT_TRUE(exist);

    st = fs_->exist("/tmp/pl_fs_nonexistent_file_xyz", &exist);
    ASSERT_FALSE(st.hasError());
    EXPECT_FALSE(exist);
}

TEST_F(PosixFileSystemTest, IsDirTrue) {
    ::mkdir("/tmp/pl_fs_test_dir", 0755);

    bool isdir = false;
    auto st = fs_->isdir("/tmp/pl_fs_test_dir", &isdir);
    ASSERT_FALSE(st.hasError());
    EXPECT_TRUE(isdir);
}

TEST_F(PosixFileSystemTest, IsDirFalseForFile) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    fd.reset();

    bool isdir = true;
    st = fs_->isdir("/tmp/pl_fs_test.file", &isdir);
    ASSERT_FALSE(st.hasError());
    EXPECT_FALSE(isdir);
}

TEST_F(PosixFileSystemTest, IsDirNonExistPath) {
    bool isdir = true;
    auto st = fs_->isdir("/tmp/pl_fs_nonexistent_path_xyz", &isdir);
    EXPECT_TRUE(st.hasError());
    EXPECT_FALSE(isdir);
}

TEST_F(PosixFileSystemTest, Rename) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    st = fs_->append(fd, 0, "rename test");
    ASSERT_FALSE(st.hasError());
    fd.reset();

    st = fs_->rename("/tmp/pl_fs_test.file", "/tmp/pl_fs_test_renamed.file");
    ASSERT_FALSE(st.hasError());

    bool exist = false;
    st = fs_->exist("/tmp/pl_fs_test.file", &exist);
    ASSERT_FALSE(st.hasError());
    EXPECT_FALSE(exist);

    st = fs_->exist("/tmp/pl_fs_test_renamed.file", &exist);
    ASSERT_FALSE(st.hasError());
    EXPECT_TRUE(exist);
}

TEST_F(PosixFileSystemTest, Mkdir) {
    // Ensure dir doesn't exist
    ::rmdir("/tmp/pl_fs_test_dir");

    auto st = fs_->mkdir("/tmp/pl_fs_test_dir", 0755);
    ASSERT_FALSE(st.hasError());

    bool isdir = false;
    st = fs_->isdir("/tmp/pl_fs_test_dir", &isdir);
    ASSERT_FALSE(st.hasError());
    EXPECT_TRUE(isdir);
}

TEST_F(PosixFileSystemTest, MkdirFails) {
    // Try to create a dir under a non-existent parent
    auto st = fs_->mkdir("/tmp/pl_fs_nonexistent_parent_xyz/subdir", 0755);
    EXPECT_TRUE(st.hasError());
}

TEST_F(PosixFileSystemTest, Remove) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    fd.reset();

    st = fs_->remove("/tmp/pl_fs_test.file");
    ASSERT_FALSE(st.hasError());

    bool exist = true;
    st = fs_->exist("/tmp/pl_fs_test.file", &exist);
    ASSERT_FALSE(st.hasError());
    EXPECT_FALSE(exist);
}

TEST_F(PosixFileSystemTest, RemoveNonExist) {
    auto st = fs_->remove("/tmp/pl_fs_nonexistent_file_xyz");
    EXPECT_TRUE(st.hasError());
}

TEST_F(PosixFileSystemTest, MtimeByPath) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    st = fs_->append(fd, 0, "mtime test");
    ASSERT_FALSE(st.hasError());
    fd.reset();

    std::time_t mtime = 0;
    st = fs_->mtime("/tmp/pl_fs_test.file", &mtime);
    ASSERT_FALSE(st.hasError());
    EXPECT_GT(mtime, 0);
}

TEST_F(PosixFileSystemTest, MtimeByPathNotExist) {
    std::time_t mtime = 0;
    auto st = fs_->mtime("/tmp/pl_fs_nonexistent_file_xyz", &mtime);
    EXPECT_TRUE(st.hasError());
}

TEST_F(PosixFileSystemTest, MtimeByFd) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    st = fs_->append(fd, 0, "mtime fd test");
    ASSERT_FALSE(st.hasError());
    st = fs_->fsync(fd, 0);
    ASSERT_FALSE(st.hasError());

    std::time_t mtime = 0;
    st = fs_->mtime(fd, &mtime);
    ASSERT_FALSE(st.hasError());
    EXPECT_GT(mtime, 0);
}

TEST_F(PosixFileSystemTest, Utime) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test_utime.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    st = fs_->append(fd, 0, "utime test");
    ASSERT_FALSE(st.hasError());
    fd.reset();

    // Set mtime to a specific time
    time_t target_time = 1000000000; // 2001-09-09
    st = fs_->utime("/tmp/pl_fs_test_utime.file", target_time);
    ASSERT_FALSE(st.hasError());

    std::time_t mtime = 0;
    st = fs_->mtime("/tmp/pl_fs_test_utime.file", &mtime);
    ASSERT_FALSE(st.hasError());
    EXPECT_EQ(target_time, mtime);
}

TEST_F(PosixFileSystemTest, UtimeWithNegativeOne) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test_utime.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    fd.reset();

    // set_time == -1 means update to current time
    st = fs_->utime("/tmp/pl_fs_test_utime.file", -1);
    ASSERT_FALSE(st.hasError());

    std::time_t mtime = 0;
    st = fs_->mtime("/tmp/pl_fs_test_utime.file", &mtime);
    ASSERT_FALSE(st.hasError());
    // mtime should be close to now
    EXPECT_NEAR(static_cast<double>(mtime), static_cast<double>(time(nullptr)), 5.0);
}

TEST_F(PosixFileSystemTest, UtimeNonExistFile) {
    auto st = fs_->utime("/tmp/pl_fs_nonexistent_file_xyz", 1000000000);
    EXPECT_TRUE(st.hasError());
}

TEST_F(PosixFileSystemTest, OpenNonExistForRead) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_nonexistent_file_xyz", O_RDONLY, &fd);
    EXPECT_TRUE(st.hasError());
}

TEST_F(PosixFileSystemTest, PreadPartial) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    st = fs_->append(fd, 0, "abcdefghij");
    ASSERT_FALSE(st.hasError());
    fd.reset();

    pl::FileDescriptorRef read_fd;
    st = fs_->open("/tmp/pl_fs_test.file", O_RDONLY, &read_fd);
    ASSERT_FALSE(st.hasError());

    // Read from offset
    char buffer[5];
    std::string_view result;
    st = fs_->pread(read_fd, 3, 5, buffer, &result);
    ASSERT_FALSE(st.hasError());
    EXPECT_EQ("defgh", result);
}

TEST_F(PosixFileSystemTest, PreadBeyondEOF) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());
    st = fs_->append(fd, 0, "short");
    ASSERT_FALSE(st.hasError());
    fd.reset();

    pl::FileDescriptorRef read_fd;
    st = fs_->open("/tmp/pl_fs_test.file", O_RDONLY, &read_fd);
    ASSERT_FALSE(st.hasError());

    // Request more bytes than available
    char buffer[100];
    std::string_view result;
    st = fs_->pread(read_fd, 0, 100, buffer, &result);
    ASSERT_FALSE(st.hasError());
    EXPECT_EQ("short", result);
    EXPECT_EQ(5, result.size());
}

TEST_F(PosixFileSystemTest, AppendMultipleTimes) {
    pl::FileDescriptorRef fd;
    auto st = fs_->open("/tmp/pl_fs_test.file", O_TRUNC | O_WRONLY | O_APPEND | O_CREAT, &fd);
    ASSERT_FALSE(st.hasError());

    st = fs_->append(fd, 0, "aaa");
    ASSERT_FALSE(st.hasError());
    st = fs_->append(fd, 0, "bbb");
    ASSERT_FALSE(st.hasError());
    st = fs_->append(fd, 0, "ccc");
    ASSERT_FALSE(st.hasError());

    st = fs_->fsync(fd, 0);
    ASSERT_FALSE(st.hasError());

    uint64_t size = 0;
    st = fs_->size(fd, &size);
    ASSERT_FALSE(st.hasError());
    EXPECT_EQ(9, size);

    fd.reset();

    // Verify content
    pl::FileDescriptorRef read_fd;
    st = fs_->open("/tmp/pl_fs_test.file", O_RDONLY, &read_fd);
    ASSERT_FALSE(st.hasError());

    char buffer[9];
    std::string_view result;
    st = fs_->pread(read_fd, 0, 9, buffer, &result);
    ASSERT_FALSE(st.hasError());
    EXPECT_EQ("aaabbbccc", result);
}

} // namespace
