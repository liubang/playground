// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "cpp/pl/sstv2/io/local_filesystem.h"

namespace pl::sstv2::io {
namespace {

std::span<const std::byte> bytes(std::string_view value) {
    return std::as_bytes(std::span(value.data(), value.size()));
}

class LocalFileSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::path(::testing::TempDir()) /
                std::filesystem::path("sstv2_local_filesystem_" +
                                      std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    [[nodiscard]] std::string path(std::string_view name) const {
        return (root_ / std::filesystem::path(name)).string();
    }

    std::filesystem::path root_;
    LocalFileSystem filesystem_;
};

TEST_F(LocalFileSystemTest, CreatesAppendsAndReadsExactly) {
    auto writer = filesystem_.create(path("table.sst"));
    ASSERT_TRUE(writer.ok()) << writer.status();
    ASSERT_TRUE(writer->valid());
    ASSERT_TRUE(filesystem_.append(*writer, bytes("hello")).ok());
    ASSERT_TRUE(filesystem_.append(*writer, bytes(" world")).ok());
    EXPECT_EQ(*filesystem_.size(*writer), 11);
    ASSERT_TRUE(filesystem_.close(*writer).ok());

    auto reader = filesystem_.open(path("table.sst"));
    ASSERT_TRUE(reader.ok()) << reader.status();
    std::string result(5, '\0');
    EXPECT_TRUE(filesystem_.read_at(*reader, 6, std::as_writable_bytes(std::span(result))).ok());
    EXPECT_EQ(result, "world");
    std::string too_long(6, '\0');
    EXPECT_FALSE(filesystem_.read_at(*reader, 6, std::as_writable_bytes(std::span(too_long))).ok());
    EXPECT_FALSE(filesystem_.append(*reader, bytes("!")).ok());
    ASSERT_TRUE(filesystem_.close(*reader).ok());
}

TEST_F(LocalFileSystemTest, EnforcesCreateOverwriteAndHandleOwnership) {
    auto writer = filesystem_.create(path("table.sst"));
    ASSERT_TRUE(writer.ok()) << writer.status();
    ASSERT_TRUE(filesystem_.append(*writer, bytes("old")).ok());
    ASSERT_TRUE(filesystem_.close(*writer).ok());
    EXPECT_FALSE(filesystem_.create(path("table.sst")).ok());

    auto replacement = filesystem_.create(path("table.sst"), CreateOptions{.overwrite = true});
    ASSERT_TRUE(replacement.ok()) << replacement.status();
    ASSERT_TRUE(filesystem_.append(*replacement, bytes("new")).ok());
    ASSERT_TRUE(filesystem_.close(*replacement).ok());

    LocalFileSystem other;
    auto reader = filesystem_.open(path("table.sst"));
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_FALSE(other.size(*reader).ok());
    ASSERT_TRUE(filesystem_.close(*reader).ok());
    EXPECT_FALSE(filesystem_.size(*reader).ok());
}

TEST_F(LocalFileSystemTest, RenamesAndRemovesFiles) {
    auto writer = filesystem_.create(path("source.sst"));
    ASSERT_TRUE(writer.ok()) << writer.status();
    ASSERT_TRUE(filesystem_.append(*writer, bytes("data")).ok());
    ASSERT_TRUE(filesystem_.close(*writer).ok());

    ASSERT_TRUE(filesystem_.rename(path("source.sst"), path("destination.sst")).ok());
    EXPECT_FALSE(filesystem_.open(path("source.sst")).ok());
    auto reader = filesystem_.open(path("destination.sst"));
    ASSERT_TRUE(reader.ok()) << reader.status();
    std::string result(4, '\0');
    ASSERT_TRUE(filesystem_.read_at(*reader, 0, std::as_writable_bytes(std::span(result))).ok());
    EXPECT_EQ(result, "data");
    ASSERT_TRUE(filesystem_.close(*reader).ok());
    ASSERT_TRUE(filesystem_.remove(path("destination.sst")).ok());
    EXPECT_FALSE(filesystem_.open(path("destination.sst")).ok());
}

} // namespace
} // namespace pl::sstv2::io
