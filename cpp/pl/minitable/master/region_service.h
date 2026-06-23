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

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "cpp/pl/minitable/master/bounded_task_queue.h"
#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

// ---------------------------------------------------------------------------
// RegionService — Region / UnitServer / Slice 调度编排服务
// ---------------------------------------------------------------------------
// 拥有 1 个独立线程 + 有界任务队列 (容量 4096).
// 负责:
//   - 心跳处理 (高频, 无需 braft)
//   - UnitServer 注册/注销 (低频, 需 braft)
//   - Slice 调度决策 (中频, 需 braft)
//
// 心跳状态 (临时, 不持久化):
//   - last_hb_us: 最近心跳时间戳
//   - state: LIVE / STALE / DEAD
//
class RegionService final : public DisableCopyAndMove {
public:
    using Task = std::function<void()>;

    explicit RegionService(size_t queue_capacity = 4096)
        : queue_(queue_capacity), worker_(&RegionService::worker_loop, this) {}

    ~RegionService() { shutdown(); }

    // 生产者: brpc 线程调用. 队列满时返回 false.
    [[nodiscard]] bool try_enqueue(Task task) { return queue_.try_enqueue(std::move(task)); }

    void shutdown() {
        queue_.shutdown();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] size_t queue_size() const { return queue_.size(); }

    // -------------------------------------------------------------------
    // 心跳状态 (临时内存状态, 不持久化)
    // -------------------------------------------------------------------

    enum class UnitServerState : uint8_t { kLive = 0, kStale = 1, kDead = 2 };

    struct UnitServerHeartbeat {
        uint64_t us_id;
        std::string host;
        uint32_t port;
        int64_t capacity;
        int64_t last_hb_us;
        UnitServerState state{UnitServerState::kLive};
    };

    // 更新心跳时间戳 (RegionService 线程调用)
    void update_heartbeat(
        uint64_t us_id, const std::string& host, uint32_t port, int64_t capacity, int64_t now_us);

    // 获取所有心跳状态 (HeartbeatMonitor 线程调用)
    [[nodiscard]] std::vector<UnitServerHeartbeat> get_all_heartbeats() const;

    // 标记 UnitServer 状态变更 (HeartbeatMonitor 线程调用)
    void set_state(uint64_t us_id, UnitServerState state);

    // 移除 UnitServer (注销后调用)
    void remove_heartbeat(uint64_t us_id);

private:
    void worker_loop() {
        while (true) {
            auto task = queue_.dequeue();
            if (!task.has_value()) {
                break;
            }
            (*task)();
        }
    }

    BoundedTaskQueue<Task> queue_;
    std::thread worker_;

    // 心跳状态 — 独立于 braft, 纯内存维护
    mutable std::mutex hb_mutex_;
    std::unordered_map<uint64_t, UnitServerHeartbeat> heartbeats_;
};

// =========================================================================
// 实现
// =========================================================================

inline void RegionService::update_heartbeat(
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
    // 心跳到达时重置为 LIVE (如果之前是 STALE)
    if (hb.state == UnitServerState::kStale) {
        hb.state = UnitServerState::kLive;
    }
    // DEAD → LIVE 在 braft on_apply 中处理 (注册恢复)
}

inline std::vector<RegionService::UnitServerHeartbeat> RegionService::get_all_heartbeats() const {
    std::lock_guard<std::mutex> lock(hb_mutex_);
    std::vector<UnitServerHeartbeat> result;
    result.reserve(heartbeats_.size());
    for (const auto& [id, hb] : heartbeats_) {
        result.push_back(hb);
    }
    return result;
}

inline void RegionService::set_state(uint64_t us_id, UnitServerState state) {
    std::lock_guard<std::mutex> lock(hb_mutex_);
    auto it = heartbeats_.find(us_id);
    if (it != heartbeats_.end()) {
        it->second.state = state;
    }
}

inline void RegionService::remove_heartbeat(uint64_t us_id) {
    std::lock_guard<std::mutex> lock(hb_mutex_);
    heartbeats_.erase(us_id);
}

} // namespace pl::minitable::master
