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
#include <source_location>

namespace pl {

class Logger {
public:
    enum class LogLevel {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        ERROR,
        FATAL,
    };

public:
    Logger(std::source_location loc);
    Logger(std::source_location loc, LogLevel level);
    LogStream& stream();

private:
    std::source_location soc_;
    std::chrono::time_point<std::chrono::system_clock> time_;
    LogLevel level_;
    LogStream stream_;
};

#define LOG_TRACE pl::Logger(std::source_location::current(), pl::Logger::LogLevel::TRACE).stream()
#define LOG_DEBUG pl::Logger(std::source_location::current(), pl::Logger::LogLevel::DEBUG).stream()
#define LOG_INFO  pl::Logger(std::source_location::current(), pl::Logger::LogLevel::INFO).stream()
#define LOG_WARN  pl::Logger(std::source_location::current(), pl::Logger::LogLevel::WARN).stream()
#define LOG_ERROR pl::Logger(std::source_location::current(), pl::Logger::LogLevel::ERROR).stream()
#define LOG_FATAL pl::Logger(std::source_location::current(), pl::Logger::LogLevel::FATAL).stream()

} // namespace pl
