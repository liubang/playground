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

#pragma once

#include "cpp/pl/log/logstream.h"

#include <chrono>
#include <string_view>

namespace pl {

class Logger {
public:
    // clang-format off
    enum class LogLevel : uint8_t {
        PL_TRACE = 0,
        PL_DEBUG = 1,
        PL_INFO  = 2,
        PL_WARN  = 3,
        PL_ERROR = 4,
        PL_FATAL = 5,
    };
    // clang-format on

    static const char* logLevel2String(LogLevel log_level) {
        switch (log_level) {
        case LogLevel::PL_TRACE:
            return "TRACE";
        case LogLevel::PL_DEBUG:
            return "DEBUG";
        case LogLevel::PL_INFO:
            return "INFO";
        case LogLevel::PL_WARN:
            return "WARN";
        case LogLevel::PL_ERROR:
            return "ERROR";
        case LogLevel::PL_FATAL:
            return "FATAL";
        }
        return "";
    }

    class SourceFile {
    public:
        template <int N> constexpr SourceFile(const char (&source)[N]) {
            source_ = source;
            auto pos = source_.find_last_of('/');
            if (pos != std::string_view::npos) {
                source_ = source_.substr(pos + 1);
            }
        }

        explicit SourceFile(const char* source) {
            source_ = source;
            auto pos = source_.find_last_of('/');
            if (pos != std::string_view::npos) {
                source_ = source_.substr(pos + 1);
            }
        }

        [[nodiscard]] constexpr std::string_view source() const { return source_; }

    private:
        std::string_view source_;
    };

    Logger(SourceFile source, int line);
    Logger(SourceFile source, int line, LogLevel level);
    ~Logger();

    LogStream& stream() { return impl_.stream_; }

private:
    class Impl {
    private:
        Impl(const Logger::SourceFile& source_file, int line, Logger::LogLevel log_level)
            : time_(std::chrono::system_clock::now()),
              source_file_(source_file),
              line_(line),
              log_level_(log_level) {
            startSession();
        }

        void startSession();
        void flush();
        std::string fmtTime();

        friend class Logger;
        std::chrono::time_point<std::chrono::system_clock> time_;
        Logger::SourceFile source_file_;
        int line_;
        Logger::LogLevel log_level_;
        LogStream stream_;
    } impl_;
};

#define __PL_LOG(level) pl::Logger(__FILE__, __LINE__, pl::Logger::LogLevel::PL##level).stream()

#define LOG(level) __PL_LOG(_##level)
#define LOG_TRACE  LOG(TRACE)
#define LOG_DEBUG  LOG(DEBUG)
#define LOG_INFO   LOG(INFO)
#define LOG_WARN   LOG(WARN)
#define LOG_ERROR  LOG(ERROR)
#define LOG_FATAL  LOG(FATAL)

} // namespace pl
