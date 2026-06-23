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
// Created: 2026/06/23 23:39

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

// =========================================================================
// 路由数据类型 — 与 protobuf 定义对齐 (见 ClusterMeta)
// =========================================================================

enum class PartitionType : uint8_t { kHash = 0, kGlobalOrder = 1 };

enum class ReplicaRole : uint8_t { kLeader = 0, kFollower = 1, kOffline = 2 };

struct ReplicaInfo {
    uint32_t replica_id;
    uint64_t us_id; // UnitServer id
    std::string host;
    uint32_t port;
    ReplicaRole role;
};

struct SliceRoute {
    uint64_t slice_id;
    std::string start_key; // memcomparable 编码
    std::string end_key;
    std::vector<ReplicaInfo> replicas;

    // 便利方法: 查找 Leader
    [[nodiscard]] const ReplicaInfo* leader() const {
        for (const auto& r : replicas) {
            if (r.role == ReplicaRole::kLeader)
                return &r;
        }
        return nullptr;
    }
};

struct TableRoute {
    uint64_t table_id;
    std::string ns;   // namespace
    std::string name; // table name
    PartitionType partition_type;

    // HASH 分区
    uint32_t slice_count{0};

    // GLOBAL_ORDER 分区 (sorted by end_key, 支持二分查找)
    std::vector<SliceRoute> slices; // HASH: indexed by hash % N
                                    // GLOBAL_ORDER: sorted by end_key
};

// 不可变只读视图 — shared_ptr 快照, 多读者无锁访问
struct RouteView {
    std::unordered_map<uint64_t, TableRoute> by_table_id;
    // 二级索引: (ns, table_name) → table_id
    std::unordered_map<std::string,                               // ns
                       std::unordered_map<std::string, uint64_t>> // name → id
        by_name;
    // 索引: us_id → 该 UnitServer 上的 Slice 列表
    std::unordered_map<uint64_t, std::vector<uint64_t>> // us_id → slice_ids
        by_unitserver;
};

// =========================================================================
// RouteTable — 高频只读路由表, shared_ptr 快照无锁读
// =========================================================================
//
// 读者 (brpc 线程, 多线程并发):
//   lookup(ns, table, key) — 无锁, 通过 atomic<shared_ptr<const RouteView>>
//
// 写者 (on_apply 线程, 单线程):
//   begin_mutation() → 返回可变副本 → 修改 → publish()
//
class RouteTable final : public DisableCopyAndMove {
public:
    RouteTable() : snapshot_(std::make_shared<const RouteView>()) {}

    // -------------------------------------------------------------------
    // 读路径 (多线程安全)
    // -------------------------------------------------------------------
    // 读者获取 shared_ptr 快照后无锁访问 RouteView (只读, 不可变).
    // shared_ptr 引用计数保证快照在读取期间不被释放.

    // 查询 (ns, table, key) 对应的 SliceRoute.
    // key 为 memcomparable 编码的 RowKey.
    [[nodiscard]] std::optional<SliceRoute> lookup(const std::string& ns,
                                                   const std::string& table,
                                                   const std::string& key) const;

    // 返回表的所有 Slice (用于全表扫描路由)
    [[nodiscard]] std::optional<std::vector<SliceRoute>> list_slices(
        const std::string& ns, const std::string& table) const;

    // 返回表的路由信息
    [[nodiscard]] std::optional<TableRoute> get_table_route(const std::string& ns,
                                                            const std::string& table) const;

    // 返回某 UnitServer 上的所有 Slice
    [[nodiscard]] std::vector<uint64_t> get_slices_by_us(uint64_t us_id) const;

    // -------------------------------------------------------------------
    // 写路径 (on_apply 线程, 单线程)
    // -------------------------------------------------------------------

    // 获取可变副本用于修改
    [[nodiscard]] std::shared_ptr<RouteView> begin_mutation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::make_shared<RouteView>(*snapshot_);
    }

    // 发布新视图 (on_apply 完成后调用)
    void publish(const std::shared_ptr<RouteView>& new_view) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = new_view;
    }

    // 获取当前快照 (多线程安全)
    [[nodiscard]] std::shared_ptr<const RouteView> get_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const RouteView> snapshot_;

    // HASH 分区: 使用 xxHash / std::hash 计算 partition key 的哈希
    static uint64_t hash_key(const std::string& key, uint32_t slice_count);
};

// =========================================================================
// 实现
// =========================================================================

inline std::optional<SliceRoute> RouteTable::lookup(const std::string& ns,
                                                    const std::string& table,
                                                    const std::string& key) const {
    auto view = get_snapshot();

    auto ns_it = view->by_name.find(ns);
    if (ns_it == view->by_name.end())
        return std::nullopt;

    auto tbl_it = ns_it->second.find(table);
    if (tbl_it == ns_it->second.end())
        return std::nullopt;

    uint64_t table_id = tbl_it->second;
    auto route_it = view->by_table_id.find(table_id);
    if (route_it == view->by_table_id.end())
        return std::nullopt;

    const auto& tr = route_it->second;

    switch (tr.partition_type) {
        case PartitionType::kHash: {
            if (tr.slices.empty())
                return std::nullopt;
            uint64_t h = hash_key(key, tr.slice_count);
            size_t idx = static_cast<size_t>(h % tr.slice_count);
            return tr.slices[idx];
        }
        case PartitionType::kGlobalOrder: {
            // 二分查找: 第一个 end_key > key 的 Slice
            // slices 按 end_key 升序排列
            auto it = std::lower_bound(
                tr.slices.begin(),
                tr.slices.end(),
                key,
                [](const SliceRoute& s, const std::string& k) { return s.end_key < k; });
            if (it != tr.slices.end()) {
                return *it;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

inline std::optional<std::vector<SliceRoute>> RouteTable::list_slices(
    const std::string& ns, const std::string& table) const {
    auto view = get_snapshot();

    auto ns_it = view->by_name.find(ns);
    if (ns_it == view->by_name.end())
        return std::nullopt;

    auto tbl_it = ns_it->second.find(table);
    if (tbl_it == ns_it->second.end())
        return std::nullopt;

    auto route_it = view->by_table_id.find(tbl_it->second);
    if (route_it == view->by_table_id.end())
        return std::nullopt;

    return route_it->second.slices;
}

inline std::optional<TableRoute> RouteTable::get_table_route(const std::string& ns,
                                                             const std::string& table) const {
    auto view = get_snapshot();

    auto ns_it = view->by_name.find(ns);
    if (ns_it == view->by_name.end())
        return std::nullopt;

    auto tbl_it = ns_it->second.find(table);
    if (tbl_it == ns_it->second.end())
        return std::nullopt;

    auto route_it = view->by_table_id.find(tbl_it->second);
    if (route_it == view->by_table_id.end())
        return std::nullopt;

    return route_it->second;
}

inline std::vector<uint64_t> RouteTable::get_slices_by_us(uint64_t us_id) const {
    auto view = get_snapshot();
    auto it = view->by_unitserver.find(us_id);
    if (it != view->by_unitserver.end()) {
        return it->second;
    }
    return {};
}

inline uint64_t RouteTable::hash_key(const std::string& key, uint32_t /*slice_count*/) {
    // 使用 std::hash, 后续可替换为 xxHash64 提升分布均匀性
    return std::hash<std::string>{}(key);
}

} // namespace pl::minitable::master
