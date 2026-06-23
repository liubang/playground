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
// Created: 2026/06/23 23:42

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

// ---------------------------------------------------------------------------
// HeartbeatMonitor — 后台心跳超时检测
// ---------------------------------------------------------------------------
// 仅在 Leader 节点运行 (on_leader_start 启动, on_leader_stop 停止).
// 周期扫描 RegionService 中的心跳时间戳, 标记超时:
//   last_hb_us > stale_threshold_us  → STALE
//   last_hb_us > dead_threshold_us   → DEAD → 回调 failover 逻辑
//
class HeartbeatMonitor final : public DisableCopyAndMove {
public:
    // 轻量心跳摘要 (避免依赖 RegionService 的内部类型)
    struct HbStatus {
        uint64_t us_id;
        int64_t last_hb_us;
        int32_t state; // 0=LIVE, 1=STALE, 2=DEAD
    };

    using FailoverCallback = std::function<void(uint64_t us_id)>;
    using ScanCallback = std::function<std::vector<HbStatus>()>;
    using SetStateCallback = std::function<void(uint64_t us_id, int32_t state)>;

    HeartbeatMonitor(int64_t stale_threshold_us,
                     int64_t dead_threshold_us,
                     int64_t check_interval_us,
                     ScanCallback do_scan,
                     SetStateCallback on_state_change,
                     FailoverCallback on_failover)
        : stale_threshold_us_(stale_threshold_us),
          dead_threshold_us_(dead_threshold_us),
          check_interval_us_(check_interval_us),
          do_scan_(std::move(do_scan)),
          on_state_change_(std::move(on_state_change)),
          on_failover_(std::move(on_failover)) {}

    ~HeartbeatMonitor() { stop(); }

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
            std::this_thread::sleep_for(std::chrono::microseconds(check_interval_us_));
            if (!running_.load(std::memory_order_relaxed))
                break;

            int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();

            for (const auto& hb : do_scan_()) {
                int64_t elapsed = now_us - hb.last_hb_us;

                if (elapsed > dead_threshold_us_ && hb.state != 2) {
                    on_state_change_(hb.us_id, 2); // kDead
                    on_failover_(hb.us_id);
                } else if (elapsed > stale_threshold_us_ && hb.state == 0) {
                    on_state_change_(hb.us_id, 1); // kStale
                }
            }
        }
    }

    int64_t stale_threshold_us_;
    int64_t dead_threshold_us_;
    int64_t check_interval_us_;
    ScanCallback do_scan_;
    SetStateCallback on_state_change_;
    FailoverCallback on_failover_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace pl::minitable::master
