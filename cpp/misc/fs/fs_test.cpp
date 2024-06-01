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

#include "cpp/misc/fs/posix_fs.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>

TEST(fs, FsWriterAndFsReader) {
    auto fs = std::make_unique<pl::PosixFs>();

    const std::string filename = "./test.log";
    pl::Status st;
    auto fw = fs->newFsWriter(filename, &st);
    EXPECT_NE(fw.get(), nullptr);
    EXPECT_TRUE(st.isOk());

    auto fr = fs->newFsReader(filename, &st);
    EXPECT_NE(fr.get(), nullptr);
    EXPECT_TRUE(st.isOk());

    pl::Binary data("hello world");
    st = fw->append(data);
    EXPECT_TRUE(st.isOk());
    st = fw->flush();
    EXPECT_TRUE(st.isOk());
    st = fw->sync();
    EXPECT_TRUE(st.isOk());
    st = fw->close();
    EXPECT_TRUE(st.isOk());

    pl::Binary result;
    char buffer[1024];
    auto status = fr->read(0, 1024, &result, buffer);
    EXPECT_TRUE(status.isOk());
    EXPECT_EQ(std::string("hello world"), std::string(result.data(), result.size()));

    ::remove(filename.data());
}
