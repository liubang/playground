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
// Created: 2026/09/12

#pragma once

#include <butil/logging.h>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace pl::mllm::server {

// butil LogSink writing every log line to a size-capped file with
// generation rotation (logrotate-style): when the active file would
// exceed max_bytes, generations shift (path.1.log <- path,
// path.2.log <- path.1.log, ...) and the oldest is dropped. Rotation
// happens inline at write time, so the log stays bounded no matter who
// launched the server or how long it runs — no external logrotate /
// launchd dependency.
class RotatingLogSink : public logging::LogSink {
public:
    struct Options {
        std::string path;                     // active log file
        int64_t max_bytes = 10 * 1024 * 1024; // rotation threshold
        int max_generations = 3;              // kept path.{1..N}.log backups
    };

    explicit RotatingLogSink(Options options);
    ~RotatingLogSink() override;

    RotatingLogSink(const RotatingLogSink&) = delete;
    RotatingLogSink& operator=(const RotatingLogSink&) = delete;

    // Appends the formatted line (flushing immediately: log volume is
    // low and crash-safety beats buffering). Returns true so butil
    // suppresses its default stderr destination — the file is THE
    // destination when this sink is installed.
    bool OnLogMessage(int severity,
                      const char* file,
                      int line,
                      const butil::StringPiece& log_content) override;

private:
    // mu_ held. Closes the active file, shifts generations, opens a
    // fresh one.
    void RotateLocked();

    Options options_;
    std::mutex mu_;
    std::ofstream out_;
    int64_t written_ = 0;
    // Set once we've told stderr the file is unusable (open/rotate failure),
    // so the warning is emitted once per sink, not per log line.
    bool warned_open_failed_ = false;
};

} // namespace pl::mllm::server
