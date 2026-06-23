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
// Created: 2026/06/23 23:44

#pragma once

#include <braft/raft.h>
#include <memory>
#include <string_view>

#include "cpp/pl/minitable/master/heartbeat_monitor.h"
#include "cpp/pl/minitable/master/region_service.h"
#include "cpp/pl/minitable/master/route_table.h"
#include "cpp/pl/minitable/master/schema_service.h"
#include "cpp/pl/minitable/master/snapshot_manager.h"
#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

// ---------------------------------------------------------------------------
// MasterSM — Master braft::StateMachine
// ---------------------------------------------------------------------------
// 串行化执行所有集群状态变更, 统筹:
//   - SchemaService  — DDL 编排 (独立线程)
//   - RegionService  — Heartbeat / US 管理 / Slice 调度 (独立线程)
//   - RouteTable     — 路由表 (on_apply 写入, 多读者无锁读取)
//
// 生命周期:
//   1. MasterSM()           — 构造
//   2. init()               — 初始化 braft Node
//   3. start(node, port)    — 加入 Raft 组, 启动服务
//   4. on_leader_start      — 启动后台线程
//   5. shutdown()           — 停止后台线程 + braft Node
//
class MasterSM : public ::braft::StateMachine {
public:
    MasterSM();
    ~MasterSM() override;

    // 不可复制/移动
    MasterSM(const MasterSM&) = delete;
    MasterSM& operator=(const MasterSM&) = delete;
    MasterSM(MasterSM&&) = delete;
    MasterSM& operator=(MasterSM&&) = delete;

    // 初始化: 创建 braft Node (不调用 start, 由调用方控制时序)
    int init(std::string_view group_id, std::string_view peer_id, std::string_view conf);

    // 启动 braft Node (通常在 brpc server 启动后调用)
    int start();

    // 关闭
    void shutdown();

    // -------------------------------------------------------------------
    // 子模块访问
    // -------------------------------------------------------------------

    SchemaService& schema_service() { return schema_service_; }
    RegionService& region_service() { return region_service_; }
    RouteTable& route_table() { return route_table_; }

    // 供 brpc Service 检查是否为 Leader (用于读写路由)
    [[nodiscard]] bool is_leader() const;

    // 供 brpc Service 提交写操作到 braft log
    [[nodiscard]] ::braft::Node* node() { return node_; }

    // -------------------------------------------------------------------
    // braft::StateMachine overrides
    // -------------------------------------------------------------------

    void on_apply(::braft::Iterator& iter) override;
    void on_snapshot_save(::braft::SnapshotWriter* writer, ::braft::Closure* done) override;
    int on_snapshot_load(::braft::SnapshotReader* reader) override;
    void on_leader_start(int64_t term) override;
    void on_leader_stop(const butil::Status& status) override;
    void on_shutdown() override;
    void on_error(const ::braft::Error& e) override;
    void on_configuration_committed(const ::braft::Configuration& conf, int64_t index) override;
    void on_start_following(const ::braft::LeaderChangeContext& ctx) override;
    void on_stop_following(const ::braft::LeaderChangeContext& ctx) override;

private:
    // 后台线程管理
    void start_background_threads();
    void stop_background_threads();

    // braft Node
    ::braft::Node* node_{nullptr};
    butil::atomic<int64_t> leader_term_{-1};

    // 子模块
    SchemaService schema_service_;
    RegionService region_service_;
    RouteTable route_table_;

    // 后台线程 (仅 Leader)
    std::unique_ptr<HeartbeatMonitor> heartbeat_monitor_;
    std::unique_ptr<SnapshotManager> snapshot_manager_;
};

} // namespace pl::minitable::master
