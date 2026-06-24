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
// Created: 2026/06/25 00:01

#include "cpp/pl/minitable/master/unit_service_impl.h"

#include <brpc/closure_guard.h>

#include <chrono>

namespace pl::minitable::master {

namespace pb = pl::minitable::proto;

namespace {

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void set_status(pb::Status* s, int code, const std::string& msg) {
    s->set_code(code);
    s->set_msg(msg);
}

void set_ok(pb::Status* s) { s->set_code(0); }

}  // namespace

void UnitServiceImpl::Heartbeat(google::protobuf::RpcController* cntl,
                                 const pb::HeartbeatRequest* req,
                                 pb::HeartbeatResponse* resp,
                                 google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl;

    if (!sm_->is_leader()) {
        set_status(resp->mutable_status(), 4, "not primary master");
        return;
    }

    uint64_t us_id = req->us_id();
    std::string host = req->host();
    uint32_t port = req->port();
    int64_t capacity = req->capacity();
    int64_t ts = now_us();

    auto* region_svc = &sm_->us_manager();
    (void)region_svc->try_enqueue_heartbeat([region_svc, us_id, host, port, capacity, ts]() {
        region_svc->update_heartbeat(us_id, host, port, capacity, ts);
    });

    set_ok(resp->mutable_status());
}

void UnitServiceImpl::RegisterUnitServer(google::protobuf::RpcController* cntl,
                                          const pb::RegisterUnitServerRequest* req,
                                          pb::RegisterUnitServerResponse* resp,
                                          google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl; (void)req;
    if (!sm_->is_leader()) {
        set_status(resp->mutable_status(), 4, "not primary master");
        return;
    }
    set_ok(resp->mutable_status());
}

}  // namespace pl::minitable::master
