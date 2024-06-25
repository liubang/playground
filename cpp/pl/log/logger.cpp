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

#include "cpp/pl/log/logger.h"
#include "cpp/pl/thread/thread.h"

#include <cstdio>
#include <fmt/chrono.h>
#include <fmt/format.h>

namespace pl {

Logger::Logger(Logger::SourceFile source, int line) : impl_(source, line, LogLevel::PL_INFO) {}

Logger::Logger(Logger::SourceFile source, int line, Logger::LogLevel log_level)
    : impl_(source, line, log_level) {}

Logger::~Logger() { impl_.flush(); }

std::string Logger::Impl::fmtTime() {
    auto now_time_t = std::chrono::system_clock::to_time_t(time_);
    std::tm now_tm = *std::localtime(&now_time_t);
    auto duration = time_.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration) % 1000;
    return fmt::format("{:%m-%d %H:%M:%S}:{:03}", now_tm, millis.count());
}

void Logger::Impl::startSession() {
    stream_ << logLevel2String(log_level_) << ": " << fmtTime() << ": * " << gettid() << " ["
            << source_file_.source() << ":" << line_ << "] ";
}

void Logger::Impl::flush() {
    stream_ << '\n';
    const auto& buffer = stream_.buffer();
    fwrite(buffer.data(), 1, buffer.size(), stdout);
}

} // namespace pl
