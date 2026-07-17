// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <gtest/gtest.h>
#include <span>
#include <string>

#include "cpp/pl/sstv2/buffer/buffer_reader.h"
#include "cpp/pl/sstv2/buffer/buffer_writer.h"

namespace pl::sstv2::buffer {
namespace {

std::string_view as_string(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

TEST(BufferReaderTest, ReadsPeeksSeeksAndPreservesPositionOnFailure) {
    auto reader = BufferReader::from_string("abcdef");
    EXPECT_EQ(reader.size(), 6);
    EXPECT_EQ(as_string(*reader.peek(2)), "ab");
    EXPECT_EQ(reader.position(), 0);
    EXPECT_EQ(as_string(*reader.read(2)), "ab");
    EXPECT_EQ(reader.position(), 2);
    EXPECT_TRUE(reader.skip(2).ok());
    EXPECT_EQ(as_string(*reader.read(2)), "ef");
    EXPECT_TRUE(reader.empty());
    EXPECT_TRUE(reader.read(0).ok());

    EXPECT_TRUE(reader.seek(1).ok());
    const auto position = reader.position();
    EXPECT_FALSE(reader.read(100).ok());
    EXPECT_EQ(reader.position(), position);
    EXPECT_FALSE(reader.seek(100).ok());
    EXPECT_EQ(reader.position(), position);
}

TEST(BufferReaderTest, CopiesHaveIndependentCursors) {
    auto first = BufferReader::from_string("abc");
    auto second = first;
    EXPECT_EQ(as_string(*first.read(1)), "a");
    EXPECT_EQ(first.position(), 1);
    EXPECT_EQ(second.position(), 0);
}

TEST(BufferWriterTest, AppendsReusesAndReleases) {
    BufferWriter writer(32);
    const auto capacity = writer.capacity();
    writer.append("abc");
    auto space = writer.append_space(2);
    space[0] = std::byte{'d'};
    space[1] = std::byte{'e'};
    EXPECT_EQ(writer.view(), "abcde");
    EXPECT_EQ(as_string(writer.bytes()), "abcde");

    writer.append(writer.bytes().subspan(1, 3));
    EXPECT_EQ(writer.view(), "abcdebcd");
    writer.clear();
    EXPECT_TRUE(writer.empty());
    EXPECT_GE(writer.capacity(), capacity);
    writer.append("done");
    EXPECT_EQ(std::move(writer).release(), "done");
}

} // namespace
} // namespace pl::sstv2::buffer
