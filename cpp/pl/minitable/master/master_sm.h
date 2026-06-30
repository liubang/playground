// Copyright (c) 2025 The Authors. All rights reserved.
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

#pragma once

#include <braft/raft.h>

#include <memory>
#include <string_view>

#include "cpp/pl/minitable/master/metadata.h"
#include "cpp/pl/minitable/master/unit_server_manager.h"

namespace pl::minitable::master {

// ---------------------------------------------------------------------------
// MasterSM — Master braft::StateMachine
// ---------------------------------------------------------------------------
// 持有:
//   - Metadata          — 集群元信息 + DDL 执行
//   - UnitServerManager — UnitServer 生命周期

class MasterSM : public ::braft::StateMachine {
public:
    MasterSM() = default;
    ~MasterSM() override = default;

    MasterSM(const MasterSM&) = delete;
    MasterSM& operator=(const MasterSM&) = delete;

    int init(std::string_view group_id, std::string_view peer_id, std::string_view conf);
    int start();
    void shutdown();

    Metadata& metadata() { return metadata_; }
    UnitServerManager& unit_server_manager() { return unit_server_manager_; }
    [[nodiscard]] bool is_leader() const;

    // braft overrides
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
    ::braft::Node* node_{nullptr};
    butil::atomic<int64_t> leader_term_{-1};
    Metadata metadata_;
    UnitServerManager unit_server_manager_;
};

}  // namespace pl::minitable::master
