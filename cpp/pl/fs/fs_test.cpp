// Copyright (c) 2024 The Authors. All rights reserved.
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

#include "cpp/pl/fs/posix_fs.h"

#include <cstdio>
#include <gtest/gtest.h>
#include <memory>

TEST(file_system, posix_filesystem) {
    pl::FileSystemPtr fs = std::make_unique<pl::PosixFileSystem>();
    pl::FileDescriptorRef fd;
    // open for write
    auto st = fs->open("/tmp/test.file", O_TRUNC | O_WRONLY | O_APPEND | O_CREAT, &fd);
    EXPECT_TRUE(st.ok());
    EXPECT_TRUE(fd != nullptr);

    st = fs->append(fd, 0, "this is test content");
    EXPECT_TRUE(st.ok());

    st = fs->fsync(fd, 0);
    EXPECT_TRUE(st.ok());

    uint64_t size;
    st = fs->size(fd, &size);
    EXPECT_TRUE(st.ok());
    EXPECT_EQ(20, size);

    bool exist = false;
    st = fs->exist("/tmp/test.file", &exist);
    EXPECT_TRUE(st.ok());
    EXPECT_TRUE(exist);

    st = fs->exist("/tmp/test.file.notfound", &exist);
    EXPECT_TRUE(st.ok());
    EXPECT_TRUE(!exist);

    // open for read
    st = fs->open("/tmp/test.file", O_RDONLY, &fd);
    EXPECT_TRUE(st.ok());
    char buffer[10];
    std::string_view result;
    st = fs->pread(fd, 0, 10, buffer, &result);
    EXPECT_TRUE(st.ok());
    EXPECT_EQ("this is te", result);

    st = fs->remove("/tmp/test.file");
    EXPECT_TRUE(st.ok());
}
