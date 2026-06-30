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

#include "cpp/pl/minitable/master/master_sm.h"

#include <braft/raft.h>
#include <braft/storage.h>
#include <braft/util.h>
#include <butil/logging.h>
#include <butil/time.h>

namespace pl::minitable::master {

// =========================================================================
// braft Node 生命周期
// =========================================================================

int MasterSM::init(std::string_view group_id, std::string_view peer_id, std::string_view conf) {
    butil::EndPoint addr;
    if (butil::str2endpoint(peer_id.data(), &addr) != 0) {
        LOG(ERROR) << "Invalid peer_id: " << peer_id;
        return -1;
    }

    ::braft::NodeOptions options;
    options.election_timeout_ms = 3000;
    options.fsm = this;
    options.snapshot_interval_s = 600;  // braft 内置 snapshot, 不需要 Checkpointer

    if (!conf.empty() && options.initial_conf.parse_from(std::string(conf)) != 0) {
        LOG(ERROR) << "Failed to parse initial_conf: " << conf;
        return -1;
    }

    node_ = new ::braft::Node(::braft::GroupId(group_id.data()), ::braft::PeerId(addr));
    if (node_->init(options) != 0) {
        LOG(ERROR) << "Failed to init braft node";
        delete node_;
        node_ = nullptr;
        return -1;
    }
    return 0;
}

int MasterSM::start() { return 0; }

void MasterSM::shutdown() {
    unit_server_manager_.stop_detector();
    if (node_) {
        node_->shutdown(nullptr);
        node_->join();
        delete node_;
        node_ = nullptr;
    }
}

bool MasterSM::is_leader() const { return node_ != nullptr && node_->is_leader(); }

// =========================================================================
// on_apply
// =========================================================================

void MasterSM::on_apply(::braft::Iterator& iter) {
    for (; iter.valid(); iter.next()) {
        // Phase 1: parse and apply mutation from iter.data()
        if (iter.done()) iter.done()->Run();
    }
}

void MasterSM::on_snapshot_save(::braft::SnapshotWriter* writer, ::braft::Closure* done) {
    ::braft::AsyncClosureGuard done_guard(done);
    (void)writer;
}

int MasterSM::on_snapshot_load(::braft::SnapshotReader* reader) {
    (void)reader;
    return 0;
}

void MasterSM::on_leader_start(int64_t term) {
    leader_term_.store(term, butil::memory_order_release);
    LOG(INFO) << "Master becomes leader, term=" << term;
    unit_server_manager_.start_detector([](uint64_t us_id) {
        LOG(WARNING) << "UnitServer " << us_id << " DEAD, triggering failover";
        // TODO: braft::apply failover task
    });
}

void MasterSM::on_leader_stop(const butil::Status& status) {
    leader_term_.store(-1, butil::memory_order_release);
    LOG(INFO) << "Master leader stop: " << status;
    unit_server_manager_.stop_detector();
}

void MasterSM::on_shutdown() {
    LOG(INFO) << "MasterSM::on_shutdown";
    unit_server_manager_.stop_detector();
}

void MasterSM::on_error(const ::braft::Error& e) {
    LOG(ERROR) << "MasterSM::on_error: " << e;
}

void MasterSM::on_configuration_committed(const ::braft::Configuration& conf, int64_t index) {
    LOG(INFO) << "MasterSM configuration committed at index=" << index;
    (void)conf;
}

void MasterSM::on_start_following(const ::braft::LeaderChangeContext& ctx) {
    LOG(INFO) << "MasterSM start following leader=" << ctx.leader_id()
              << " term=" << ctx.term();
}

void MasterSM::on_stop_following(const ::braft::LeaderChangeContext& ctx) {
    LOG(INFO) << "MasterSM stop following, leader_id=" << ctx.leader_id()
              << " status=" << ctx.status();
}

}  // namespace pl::minitable::master
