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
// Created: 2026/06/25 00:00

#include "cpp/pl/minitable/master/master_service_impl.h"

#include <brpc/closure_guard.h>

#include <string>

#include "cpp/pl/minitable/master/metadata.h"

namespace pl::minitable::master {

namespace pb = pl::minitable::proto;

namespace {

using pb::Status;

void set_status(Status* s, int code, const std::string& msg) {
    s->set_code(code);
    s->set_msg(msg);
}

void set_ok(Status* s) { s->set_code(0); }

pb::ReplicaRole to_proto_role(master::ReplicaRole r) {
    switch (r) {
    case master::ReplicaRole::kPrimary:   return pb::PRIMARY;
    case master::ReplicaRole::kSecondary: return pb::SECONDARY;
    default:                              return pb::OFFLINE;
    }
}

void fill_slice_info(pb::SliceInfo* info, const SliceRoute& route) {
    info->set_slice_id(route.slice_id);
    info->set_start_key(route.start_key);
    info->set_end_key(route.end_key);
    for (const auto& r : route.replicas) {
        auto* ep = info->add_replicas();
        ep->set_replica_id(r.replica_id);
        ep->set_us_id(r.us_id);
        ep->set_host(r.host);
        ep->set_port(r.port);
        ep->set_role(to_proto_role(r.role));
    }
}

}  // namespace

// =========================================================================
// GetSlice
// =========================================================================

void MasterServiceImpl::GetSlice(google::protobuf::RpcController* cntl,
                                  const pb::GetSliceRequest* req,
                                  pb::GetSliceResponse* resp,
                                  google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl;

    if (req->namespace_().empty() || req->table_name().empty()) {
        set_status(resp->mutable_status(), 1, "namespace and table_name required");
        return;
    }

    if (req->row_key().values_size() > 0) {
        std::string encoded;
        for (const auto& v : req->row_key().values()) encoded.append(v);
        auto route = sm_->metadata().lookup(req->namespace_(), req->table_name(), encoded);
        if (!route.has_value()) {
            set_status(resp->mutable_status(), 2, "table not found or key out of range");
            return;
        }
        fill_slice_info(resp->add_slices(), *route);
    } else {
        auto routes = sm_->metadata().list_slices(req->namespace_(), req->table_name());
        if (!routes.has_value()) {
            set_status(resp->mutable_status(), 2, "table not found");
            return;
        }
        for (const auto& route : *routes) {
            fill_slice_info(resp->add_slices(), route);
        }
    }
    set_ok(resp->mutable_status());
}

// =========================================================================
// CreateTable
// =========================================================================

void MasterServiceImpl::CreateTable(google::protobuf::RpcController* cntl,
                                     const pb::CreateTableRequest* req,
                                     pb::CreateTableResponse* resp,
                                     google::protobuf::Closure* done) {
    (void)cntl;

    if (req->table_name().empty() || !req->has_schema()) {
        brpc::ClosureGuard done_guard(done);
        set_status(resp->mutable_status(), 1, "table_name and schema required");
        return;
    }
    if (!sm_->is_leader()) {
        brpc::ClosureGuard done_guard(done);
        set_status(resp->mutable_status(), 4, "not primary master");
        return;
    }

    pb::CreateTableRequest creq = *req;
    auto* resp_ptr = resp;
    auto* done_ptr = done;

    auto enqueued = sm_->metadata().try_enqueue_ddl([creq, resp_ptr, done_ptr]() {
        set_ok(resp_ptr->mutable_status());
        resp_ptr->mutable_table()->set_table_id(0);
        done_ptr->Run();
    });

    if (!enqueued.has_value()) {
        brpc::ClosureGuard done_guard(done);
        set_status(resp->mutable_status(), 3, "schema service overloaded");
    }
}

// =========================================================================
// Stubs
// =========================================================================

void MasterServiceImpl::UpdateTable(google::protobuf::RpcController* cntl,
                                     const pb::UpdateTableRequest* req,
                                     pb::UpdateTableResponse* resp,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl; (void)req;
    if (!sm_->is_leader()) { set_status(resp->mutable_status(), 4, "not primary master"); return; }
    set_ok(resp->mutable_status());
}

void MasterServiceImpl::DropTable(google::protobuf::RpcController* cntl,
                                   const pb::DropTableRequest* req,
                                   pb::DropTableResponse* resp,
                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl; (void)req;
    if (!sm_->is_leader()) { set_status(resp->mutable_status(), 4, "not primary master"); return; }
    set_ok(resp->mutable_status());
}

void MasterServiceImpl::GetTable(google::protobuf::RpcController* cntl,
                                  const pb::GetTableRequest* req,
                                  pb::GetTableResponse* resp,
                                  google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl;
    auto tr = sm_->metadata().get_table_route(req->namespace_(), req->table_name());
    if (!tr.has_value()) {
        set_status(resp->mutable_status(), 2, "table not found");
        return;
    }
    resp->mutable_table()->set_table_id(tr->table_id);
    resp->mutable_table()->set_namespace_(tr->ns);
    resp->mutable_table()->set_name(tr->name);
    set_ok(resp->mutable_status());
}

void MasterServiceImpl::ListTables(google::protobuf::RpcController* cntl,
                                    const pb::ListTablesRequest* req,
                                    pb::ListTablesResponse* resp,
                                    google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl; (void)req;
    set_ok(resp->mutable_status());
}

}  // namespace pl::minitable::master
