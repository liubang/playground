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

#include "cpp/pl/minitable/master/master.h"

#include <brpc/closure_guard.h>
#include <chrono>
#include <string>

#include "cpp/pl/minitable/proto/admin.pb.h"
#include "cpp/pl/minitable/proto/master.pb.h"
#include "cpp/pl/minitable/proto/unit.pb.h"

namespace pl::minitable::master {

namespace {

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

namespace {

void fill_slice_info(proto::SliceInfo* info, const SliceRoute& route) {
    info->set_slice_id(route.slice_id);
    info->set_start_key(route.start_key);
    info->set_end_key(route.end_key);
    for (const auto& r : route.replicas) {
        auto* ep = info->add_replicas();
        ep->set_replica_id(r.replica_id);
        ep->set_us_id(r.us_id);
        ep->set_host(r.host);
        ep->set_port(r.port);
        ep->set_role(static_cast<int32_t>(r.role));
    }
}

} // namespace

// =========================================================================
// GetSlice — 直读 RouteTable, brpc 线程内完成 (无排队)
// =========================================================================

void MasterServiceImpl::GetSlice(google::protobuf::RpcController* cntl,
                                 const proto::GetSliceRequest* req,
                                 proto::GetSliceResponse* resp,
                                 google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl;

    if (req->namespace_().empty() || req->table_name().empty()) {
        resp->set_error_code(1);
        resp->set_error_msg("namespace and table_name are required");
        return;
    }

    if (!req->row_key().empty()) {
        // 按 row_key 定位单个 Slice
        auto route =
            sm_->route_table().lookup(req->namespace_(), req->table_name(), req->row_key());
        if (!route.has_value()) {
            resp->set_error_code(2);
            resp->set_error_msg("table not found or key out of range");
            return;
        }
        fill_slice_info(resp->add_slices(), *route);
    } else {
        // 返回表的所有 Slice
        auto routes = sm_->route_table().list_slices(req->namespace_(), req->table_name());
        if (!routes.has_value()) {
            resp->set_error_code(2);
            resp->set_error_msg("table not found");
            return;
        }
        for (const auto& route : *routes) {
            fill_slice_info(resp->add_slices(), route);
        }
    }
}

// =========================================================================
// CreateTable — dispatch → SchemaService (异步: 等待 braft commit 后响应)
// =========================================================================

void MasterServiceImpl::CreateTable(google::protobuf::RpcController* cntl,
                                    const proto::CreateTableRequest* req,
                                    proto::CreateTableResponse* resp,
                                    google::protobuf::Closure* done) {
    (void)cntl;

    // 参数校验
    if (req->table_name().empty() || !req->has_schema()) {
        brpc::ClosureGuard done_guard(done);
        resp->set_error_code(1);
        resp->set_error_msg("table_name and schema are required");
        return;
    }

    if (!sm_->is_leader()) {
        brpc::ClosureGuard done_guard(done);
        resp->set_error_code(4);
        resp->set_error_msg("not leader");
        return;
    }

    // 异步: release ClosureGuard, lambda 中调用 done->Run()
    proto::CreateTableRequest creq = *req;
    auto* resp_ptr = resp;
    auto* done_ptr = done;

    bool enqueued = sm_->schema_service().try_enqueue([creq, resp_ptr, done_ptr]() {
        // Phase 1 stub: 校验 TableSchema 合法性 (row_keys 非空, LG 非空, 类型检查)
        const auto& schema = creq.schema();
        resp_ptr->set_error_code(0);
        resp_ptr->set_table_id(0); // TODO: 真实分配
        done_ptr->Run();
    });

    if (!enqueued) {
        brpc::ClosureGuard done_guard(done);
        resp->set_error_code(3);
        resp->set_error_msg("schema service overloaded, please retry");
    }
}

// =========================================================================
// Heartbeat — dispatch → RegionService (fire-and-forget, 立即响应)
// =========================================================================

void MasterServiceImpl::Heartbeat(google::protobuf::RpcController* cntl,
                                  const proto::HeartbeatRequest* req,
                                  proto::HeartbeatResponse* resp,
                                  google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl;

    if (!sm_->is_leader()) {
        resp->set_error_code(4);
        return;
    }

    uint64_t us_id = req->us_id();
    std::string host = req->host();
    uint32_t port = req->port();
    int64_t capacity = req->capacity();
    int64_t ts = now_us();

    auto* region_svc = &sm_->region_service();
    (void)region_svc->try_enqueue([region_svc, us_id, host, port, capacity, ts]() {
        region_svc->update_heartbeat(us_id, host, port, capacity, ts);
    });
    // 心跳是 fire-and-forget: 入队失败也正常返回 (下一条心跳会覆盖)

    resp->set_error_code(0);
}

// =========================================================================
// DropTable / CreateRegion / DropRegion / RegisterUnitServer — stubs
// =========================================================================

void MasterServiceImpl::DropTable(google::protobuf::RpcController* cntl,
                                  const proto::DropTableRequest* req,
                                  proto::DropTableResponse* resp,
                                  google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl;
    (void)req;
    if (!sm_->is_leader()) {
        resp->set_error_code(4);
        resp->set_error_msg("not leader");
        return;
    }
    resp->set_error_code(0);
}

void MasterServiceImpl::CreateRegion(google::protobuf::RpcController* cntl,
                                     const proto::CreateRegionRequest* req,
                                     proto::CreateRegionResponse* resp,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl;
    (void)req;
    if (!sm_->is_leader()) {
        resp->set_error_code(4);
        resp->set_error_msg("not leader");
        return;
    }
    resp->set_error_code(0);
}

void MasterServiceImpl::DropRegion(google::protobuf::RpcController* cntl,
                                   const proto::DropRegionRequest* req,
                                   proto::DropRegionResponse* resp,
                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl;
    (void)req;
    if (!sm_->is_leader()) {
        resp->set_error_code(4);
        resp->set_error_msg("not leader");
        return;
    }
    resp->set_error_code(0);
}

void MasterServiceImpl::RegisterUnitServer(google::protobuf::RpcController* cntl,
                                           const proto::RegisterUnitServerRequest* req,
                                           proto::RegisterUnitServerResponse* resp,
                                           google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl;
    (void)req;
    if (!sm_->is_leader()) {
        resp->set_error_code(4);
        resp->set_error_msg("not leader");
        return;
    }
    resp->set_error_code(0);
}

} // namespace pl::minitable::master
