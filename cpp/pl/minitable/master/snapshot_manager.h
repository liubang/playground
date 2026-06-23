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
// Created: 2026/06/23 23:43

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

// ---------------------------------------------------------------------------
// SnapshotManager — 后台定期触发 braft snapshot
// ---------------------------------------------------------------------------
// 仅在 Leader 节点运行.
// 触发条件:
//   - 每隔 snapshot_interval_us (默认 10 分钟)
//   - braft log 条数超过 log_count_threshold (由 braft 配置)
//
class SnapshotManager final : public DisableCopyAndMove {
public:
    using SnapshotFn = std::function<void()>;

    SnapshotManager(int64_t interval_us, SnapshotFn do_snapshot)
        : interval_us_(interval_us), do_snapshot_(std::move(do_snapshot)) {}

    ~SnapshotManager() { stop(); }

    void start() {
        if (running_.exchange(true, std::memory_order_acq_rel))
            return;
        thread_ = std::thread([this] { run_loop(); });
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel))
            return;
        if (thread_.joinable())
            thread_.join();
    }

private:
    void run_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::microseconds(interval_us_));
            if (!running_.load(std::memory_order_relaxed))
                break;
            do_snapshot_();
        }
    }

    int64_t interval_us_;
    SnapshotFn do_snapshot_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace pl::minitable::master
