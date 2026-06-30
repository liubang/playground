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

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/pl/minitable/proto/common.pb.h"
#include "cpp/pl/thread/thread_pool.h"
#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

namespace pb = pl::minitable::proto;

// =========================================================================
// TableEntry — TableInfo + Slice 组合, proto 中没有现成等价物
// =========================================================================

struct TableEntry {
    pb::TableInfo info;
    std::vector<pb::SliceInfo> slices;

    [[nodiscard]] const pb::ReplicaEndpoint* primary() const {
        for (const auto& s : slices) {
            for (const auto& r : s.replicas()) {
                if (r.role() == pb::ReplicaRole::PRIMARY) return &r;
            }
        }
        return nullptr;
    }
};

// 不可变只读视图 — shared_ptr 快照, 多读者无锁访问
struct ClusterView {
    std::unordered_map<uint64_t, std::shared_ptr<const TableEntry>> by_table_id;
    std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> by_name;
    std::unordered_map<uint64_t, std::vector<uint64_t>> by_unitserver;
};

// =========================================================================
// Metadata — 集群内存状态 + DDL 异步执行
// =========================================================================
//
// 职责:
//   - 维护 Table/Slice/Replica 索引 (COW 快照, 无锁读)
//   - DDL 任务队列 (ThreadPool 1 线程, 容量 64, 满时拒绝)

class Metadata final : public DisableCopyAndMove {
public:
    Metadata() : snapshot_(std::make_shared<const ClusterView>()) {}

    // ---- 状态查询 (多线程安全) ----

    [[nodiscard]] std::optional<pb::SliceInfo> lookup(const std::string& ns,
                                                       const std::string& table,
                                                       const std::string& key) const;

    [[nodiscard]] std::optional<std::vector<pb::SliceInfo>> list_slices(
        const std::string& ns, const std::string& table) const;

    [[nodiscard]] std::optional<pb::TableInfo> get_table(const std::string& ns,
                                                          const std::string& table) const;

    [[nodiscard]] std::vector<uint64_t> get_slices_by_us(uint64_t unit_server_id) const;

    [[nodiscard]] std::shared_ptr<const ClusterView> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    // ---- 状态变更 (on_apply 单线程) ----

    [[nodiscard]] std::shared_ptr<ClusterView> begin_mutation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::make_shared<ClusterView>(*snapshot_);
    }

    void publish(const std::shared_ptr<ClusterView>& view) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = view;
    }

    // ---- DDL (多生产者, 1 消费者) ----

    [[nodiscard]] auto try_enqueue_ddl(std::function<void()> task)
        -> std::optional<std::future<void>> {
        return ddl_pool_.try_submit(std::move(task));
    }

    [[nodiscard]] size_t ddl_queue_size() const { return ddl_pool_.pending(); }

private:
    static uint64_t hash_key(const std::string& key, uint32_t slice_count);

    mutable std::mutex mutex_;
    std::shared_ptr<const ClusterView> snapshot_;
    ThreadPool ddl_pool_{1, 64};
};

// =========================================================================
// 实现
// =========================================================================

inline std::optional<pb::SliceInfo> Metadata::lookup(const std::string& ns,
                                                      const std::string& table,
                                                      const std::string& key) const {
    auto view = snapshot();
    auto ns_it = view->by_name.find(ns);
    if (ns_it == view->by_name.end()) return std::nullopt;
    auto tbl_it = ns_it->second.find(table);
    if (tbl_it == ns_it->second.end()) return std::nullopt;
    auto route_it = view->by_table_id.find(tbl_it->second);
    if (route_it == view->by_table_id.end()) return std::nullopt;

    const auto& entry = *route_it->second;
    switch (entry.info.partition().type()) {
    case pb::PartitionType::HASH:
        if (entry.slices.empty()) return std::nullopt;
        return entry.slices[hash_key(key, entry.info.partition().slice_count()) %
                            entry.info.partition().slice_count()];
    case pb::PartitionType::GLOBAL_ORDER: {
        auto it = std::lower_bound(entry.slices.begin(), entry.slices.end(), key,
                                    [](const pb::SliceInfo& s, const std::string& k) {
                                        return s.end_key() < k;
                                    });
        if (it != entry.slices.end()) return *it;
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

inline std::optional<std::vector<pb::SliceInfo>> Metadata::list_slices(
    const std::string& ns, const std::string& table) const {
    auto view = snapshot();
    auto ns_it = view->by_name.find(ns);
    if (ns_it == view->by_name.end()) return std::nullopt;
    auto tbl_it = ns_it->second.find(table);
    if (tbl_it == ns_it->second.end()) return std::nullopt;
    auto route_it = view->by_table_id.find(tbl_it->second);
    if (route_it == view->by_table_id.end()) return std::nullopt;
    return route_it->second->slices;
}

inline std::optional<pb::TableInfo> Metadata::get_table(const std::string& ns,
                                                         const std::string& table) const {
    auto view = snapshot();
    auto ns_it = view->by_name.find(ns);
    if (ns_it == view->by_name.end()) return std::nullopt;
    auto tbl_it = ns_it->second.find(table);
    if (tbl_it == ns_it->second.end()) return std::nullopt;
    auto route_it = view->by_table_id.find(tbl_it->second);
    if (route_it == view->by_table_id.end()) return std::nullopt;
    return route_it->second->info;
}

inline std::vector<uint64_t> Metadata::get_slices_by_us(uint64_t unit_server_id) const {
    auto view = snapshot();
    auto it = view->by_unitserver.find(unit_server_id);
    if (it != view->by_unitserver.end()) return it->second;
    return {};
}

inline uint64_t Metadata::hash_key(const std::string& key, uint32_t /*slice_count*/) {
    return std::hash<std::string>{}(key);
}

}  // namespace pl::minitable::master
