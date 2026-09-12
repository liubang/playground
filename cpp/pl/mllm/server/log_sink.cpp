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

#include "cpp/pl/mllm/server/log_sink.h"

#include <cstdio>
#include <sstream>
#include <sys/stat.h>
#include <utility>

namespace pl::mllm::server {

namespace {

std::string GenerationPath(const std::string& base, int generation) {
    return base + "." + std::to_string(generation) + ".log";
}

int64_t FileSize(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<int64_t>(st.st_size);
}

} // namespace

RotatingLogSink::RotatingLogSink(Options options) : options_(std::move(options)) {
    // Append across restarts; the size cap is enforced on the next write.
    written_ = FileSize(options_.path);
    out_.open(options_.path, std::ios::app);
}

RotatingLogSink::~RotatingLogSink() {
    std::lock_guard<std::mutex> lock(mu_);
    out_.flush();
}

bool RotatingLogSink::OnLogMessage(int severity,
                                   const char* file,
                                   int line,
                                   const butil::StringPiece& log_content) {
    std::ostringstream formatted;
    logging::PrintLog(formatted, severity, file, line, log_content);
    std::string text = formatted.str();
    // PrintLog emits no line terminator; the sink owns framing.
    if (text.empty() || text.back() != '\n') {
        text.push_back('\n');
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (!out_.is_open()) {
        return true; // sink installed but file unusable: drop, don't spam stderr
    }
    if (written_ > 0 && written_ + static_cast<int64_t>(text.size()) > options_.max_bytes) {
        RotateLocked();
    }
    out_ << text;
    out_.flush();
    written_ += static_cast<int64_t>(text.size());
    return true;
}

void RotatingLogSink::RotateLocked() {
    out_.close();
    // The oldest generation is dropped; the rest shift up by one.
    std::remove(GenerationPath(options_.path, options_.max_generations).c_str());
    for (int i = options_.max_generations - 1; i >= 1; --i) {
        std::rename(GenerationPath(options_.path, i).c_str(),
                    GenerationPath(options_.path, i + 1).c_str());
    }
    std::rename(options_.path.c_str(), GenerationPath(options_.path, 1).c_str());
    out_.open(options_.path, std::ios::trunc);
    written_ = 0;
}

} // namespace pl::mllm::server
