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
// Created: 2026/09/13

#include <atomic>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

#include "cpp/pl/mllm/server/log_sink.h"

namespace pl::mllm::server {
namespace {

// RAII temp directory per test.
class TempDir {
public:
    TempDir() {
        static std::atomic<uint64_t> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mllm_log_sink_test_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string file(const std::string& name) const {
        return (path_ / name).string();
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool FileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

// Feeds the sink a numbered log line; returns the sink's acceptance flag.
bool Log(RotatingLogSink& sink, int n) {
    const std::string content = "log line " + std::to_string(n);
    return sink.OnLogMessage(0 /*INFO*/, "log_sink_test.cpp", 42, butil::StringPiece(content));
}

TEST(RotatingLogSinkTest, WritesFormattedLinesWithTerminator) {
    TempDir dir;
    const std::string file = dir.file("app.log");
    {
        RotatingLogSink sink({.path = file, .max_bytes = 1 << 20, .max_generations = 3});
        // Last-write-wins destination: returns true so butil won't also emit.
        EXPECT_TRUE(
            sink.OnLogMessage(0, "log_sink_test.cpp", 7, butil::StringPiece("hello world")));
    }
    const std::string text = ReadFile(file);
    EXPECT_NE(text.find("hello world"), std::string::npos);
    EXPECT_NE(text.find("log_sink_test.cpp:7"), std::string::npos); // severity/file/line framing
    EXPECT_EQ(text.back(), '\n'); // sink owns framing even without input terminator
}

TEST(RotatingLogSinkTest, RotatesAtCapAndShiftsGenerations) {
    TempDir dir;
    const std::string file = dir.file("app.log");
    // Tiny cap: each formatted line is tens of bytes, so ~40 lines trigger
    // several rotations at a 400-byte cap.
    {
        RotatingLogSink sink({.path = file, .max_bytes = 400, .max_generations = 2});
        for (int i = 0; i < 40; ++i) {
            ASSERT_TRUE(Log(sink, i));
        }
    }
    EXPECT_TRUE(FileExists(file));             // active generation
    EXPECT_TRUE(FileExists(file + ".1.log"));  // previous generation
    EXPECT_TRUE(FileExists(file + ".2.log"));  // oldest kept
    EXPECT_FALSE(FileExists(file + ".3.log")); // beyond cap -> dropped

    // The active file holds the most recent line and every generation is
    // bounded by the cap plus at most one oversized line.
    const std::string active = ReadFile(file);
    EXPECT_NE(active.find("log line 39"), std::string::npos);
    EXPECT_LE(std::filesystem::file_size(file), 400 + 128);
    EXPECT_LE(std::filesystem::file_size(file + ".1.log"), 400 + 128);
    EXPECT_LE(std::filesystem::file_size(file + ".2.log"), 400 + 128);
}

TEST(RotatingLogSinkTest, AppendsAcrossRestartsThenCaps) {
    TempDir dir;
    const std::string file = dir.file("app.log");
    {
        RotatingLogSink sink({.path = file, .max_bytes = 400, .max_generations = 2});
        ASSERT_TRUE(Log(sink, 0));
        ASSERT_TRUE(Log(sink, 1));
    }
    const uintmax_t size_before = std::filesystem::file_size(file);
    {
        // Simulated restart: the existing file size must seed the rotation
        // budget (no truncation, no unbounded growth across restarts).
        RotatingLogSink sink({.path = file, .max_bytes = 400, .max_generations = 2});
        ASSERT_TRUE(Log(sink, 2));
    }
    EXPECT_GE(std::filesystem::file_size(file), size_before);
    const std::string text = ReadFile(file);
    EXPECT_NE(text.find("log line 0"), std::string::npos);
    EXPECT_NE(text.find("log line 2"), std::string::npos);
}

TEST(RotatingLogSinkTest, UnopenablePathStillAcceptsLogsWithoutCrashing) {
    TempDir dir;
    // Parent directory does not exist -> open fails; the sink must stay
    // silent (accept + drop, one stderr warning from the constructor) rather
    // than spam stderr per line or crash.
    const std::string file = dir.file("no/such/dir/app.log");
    RotatingLogSink sink({.path = file, .max_bytes = 400, .max_generations = 2});
    EXPECT_TRUE(Log(sink, 0));
    EXPECT_TRUE(Log(sink, 1));
    EXPECT_FALSE(FileExists(file));
    EXPECT_TRUE(std::filesystem::exists(dir.path())); // nothing odd created
}

TEST(RotatingLogSinkTest, DegenerateConfigIsClamped) {
    TempDir dir;
    const std::string file = dir.file("app.log");
    // max_generations = 0 would talk itself into endless rotation work; the
    // constructor clamps it, so rotation still lands in .1.log.
    {
        RotatingLogSink sink({.path = file, .max_bytes = 200, .max_generations = 0});
        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE(Log(sink, i));
        }
    }
    EXPECT_TRUE(FileExists(file));
    EXPECT_TRUE(FileExists(file + ".1.log"));
}

} // namespace
} // namespace pl::mllm::server
