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
// Created: 2026/06/25 00:28

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cpp/pl/thread/thread_pool.h"
#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

// =========================================================================
// UnitServerManager — UnitServer 生命周期管理
// =========================================================================
//
// 职责:
//   - 心跳处理 (ThreadPool 1 线程, 容量 4096, fire-and-forget)
//   - 存活状态追踪 (纯内存, 不持久化)
//   - 故障检测 (后台线程, 周期扫描, 触发 failover)

class UnitServerManager final : public DisableCopyAndMove {
public:
    enum class State : uint8_t { kLive = 0, kStale = 1, kDead = 2 };

    struct Heartbeat {
        uint64_t us_id;
        std::string host;
        uint32_t port;
        int64_t capacity;
        int64_t last_hb_us;
        State state{State::kLive};
    };

    using FailoverFn = std::function<void(uint64_t us_id)>;

    explicit UnitServerManager(size_t queue_capacity = 4096,
                               int64_t stale_threshold_us = 10'000'000,
                               int64_t dead_threshold_us = 30'000'000,
                               int64_t check_interval_us = 1'000'000)
        : pool_(1, queue_capacity),
          stale_threshold_us_(stale_threshold_us),
          dead_threshold_us_(dead_threshold_us),
          check_interval_us_(check_interval_us) {}

    ~UnitServerManager() { stop_detector(); }

    // ---- 心跳处理 ----

    [[nodiscard]] auto try_enqueue_heartbeat(std::function<void()> task)
        -> std::optional<std::future<void>> {
        return pool_.try_submit(std::move(task));
    }

    [[nodiscard]] size_t queue_size() const { return pool_.pending(); }

    // ---- 存活状态 ----

    void update_heartbeat(
        uint64_t us_id, const std::string& host, uint32_t port, int64_t capacity, int64_t now_us);

    [[nodiscard]] std::vector<Heartbeat> get_all_heartbeats() const;

    void set_state(uint64_t us_id, State state);
    void remove_heartbeat(uint64_t us_id);

    // ---- 故障检测 ----

    void start_detector(FailoverFn on_failover);
    void stop_detector();

private:
    void detector_loop(FailoverFn on_failover);

    ThreadPool pool_{1, 4096};
    mutable std::mutex hb_mutex_;
    std::unordered_map<uint64_t, Heartbeat> heartbeats_;

    int64_t stale_threshold_us_;
    int64_t dead_threshold_us_;
    int64_t check_interval_us_;
    std::atomic<bool> detector_running_{false};
    std::thread detector_thread_;
};

// =========================================================================
// 实现
// =========================================================================

inline void UnitServerManager::update_heartbeat(
    uint64_t us_id, const std::string& host, uint32_t port, int64_t capacity, int64_t now_us) {
    std::lock_guard<std::mutex> lock(hb_mutex_);
    auto [it, inserted] = heartbeats_.try_emplace(us_id);
    auto& hb = it->second;
    if (inserted) {
        hb.us_id = us_id;
        hb.host = host;
        hb.port = port;
        hb.capacity = capacity;
    }
    hb.last_hb_us = now_us;
    if (hb.state == State::kStale) {
        hb.state = State::kLive;
    }
}

inline std::vector<UnitServerManager::Heartbeat> UnitServerManager::get_all_heartbeats() const {
    std::lock_guard<std::mutex> lock(hb_mutex_);
    std::vector<Heartbeat> result;
    result.reserve(heartbeats_.size());
    for (const auto& [id, hb] : heartbeats_)
        result.push_back(hb);
    return result;
}

inline void UnitServerManager::set_state(uint64_t us_id, State state) {
    std::lock_guard<std::mutex> lock(hb_mutex_);
    auto it = heartbeats_.find(us_id);
    if (it != heartbeats_.end())
        it->second.state = state;
}

inline void UnitServerManager::remove_heartbeat(uint64_t us_id) {
    std::lock_guard<std::mutex> lock(hb_mutex_);
    heartbeats_.erase(us_id);
}

inline void UnitServerManager::start_detector(FailoverFn on_failover) {
    if (detector_running_.exchange(true))
        return;
    detector_thread_ = std::thread([this, fn = std::move(on_failover)] {
        while (detector_running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::microseconds(check_interval_us_));
            if (!detector_running_.load(std::memory_order_relaxed))
                break;

            int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();

            for (const auto& hb : get_all_heartbeats()) {
                int64_t elapsed = now_us - hb.last_hb_us;
                if (elapsed > dead_threshold_us_ && hb.state != State::kDead) {
                    set_state(hb.us_id, State::kDead);
                    fn(hb.us_id);
                } else if (elapsed > stale_threshold_us_ && hb.state == State::kLive) {
                    set_state(hb.us_id, State::kStale);
                }
            }
        }
    });
}

inline void UnitServerManager::stop_detector() {
    if (!detector_running_.exchange(false))
        return;
    if (detector_thread_.joinable())
        detector_thread_.join();
}

} // namespace pl::minitable::master
