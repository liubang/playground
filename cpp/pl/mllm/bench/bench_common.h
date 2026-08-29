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
// Created: 2026/08/29 22:15

#pragma once

// Shared helpers for the mllm benchmark binaries (bench_decode, bench_ops).
// Pure C++ / POSIX only — deliberately no dependency on engine.h so both
// binaries can share it without pulling in the engine target.

#include <cstdio>
#include <string>
#include <sys/resource.h>

namespace pl::mllm::bench {

// Peak resident set size of the current process, in bytes.
// macOS reports ru_maxrss in bytes; Linux reports kilobytes — normalize.
inline long peak_rss_bytes() {
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return ru.ru_maxrss; // bytes on Darwin
#else
    return ru.ru_maxrss * 1024; // kilobytes on Linux
#endif
}

// Current git HEAD commit (empty string when not in a git worktree or git is
// unavailable). Best-effort, for provenance in benchmark output.
inline std::string git_commit() {
    std::string out;
    FILE* pipe = popen("git rev-parse --short=12 HEAD 2>/dev/null", "r");
    if (pipe != nullptr) {
        char buf[64];
        if (fgets(buf, sizeof(buf), pipe) != nullptr) {
            out = buf;
            // Trim trailing newline.
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
                out.pop_back();
            }
        }
        pclose(pipe);
    }
    return out;
}

} // namespace pl::mllm::bench
