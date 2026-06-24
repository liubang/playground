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

#include "cpp/pl/minitable/master/admin_service_impl.h"

#include <brpc/closure_guard.h>

namespace pl::minitable::master {

namespace pb = pl::minitable::proto;

namespace {

void set_status(pb::Status* s, int code, const std::string& msg) {
    s->set_code(code);
    s->set_msg(msg);
}

void set_ok(pb::Status* s) { s->set_code(0); }

}  // namespace

#define CHECK_LEADER(r)                                                     \
    do {                                                                    \
        if (!sm_->is_leader()) {                                            \
            set_status((r)->mutable_status(), 4, "not primary master");     \
            return;                                                         \
        }                                                                   \
    } while (0)

void AdminServiceImpl::CreateRegion(google::protobuf::RpcController* cntl,
                                     const pb::CreateRegionRequest* req,
                                     pb::CreateRegionResponse* resp,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl; (void)req;
    CHECK_LEADER(resp);
    set_ok(resp->mutable_status());
}

void AdminServiceImpl::DropRegion(google::protobuf::RpcController* cntl,
                                   const pb::DropRegionRequest* req,
                                   pb::DropRegionResponse* resp,
                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl; (void)req;
    CHECK_LEADER(resp);
    set_ok(resp->mutable_status());
}

void AdminServiceImpl::GetRegion(google::protobuf::RpcController* cntl,
                                  const pb::GetRegionRequest* req,
                                  pb::GetRegionResponse* resp,
                                  google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl; (void)req;
    set_ok(resp->mutable_status());
}

void AdminServiceImpl::ListRegions(google::protobuf::RpcController* cntl,
                                    const pb::ListRegionsRequest* req,
                                    pb::ListRegionsResponse* resp,
                                    google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl; (void)req;
    set_ok(resp->mutable_status());
}

void AdminServiceImpl::ListUnitServers(google::protobuf::RpcController* cntl,
                                        const pb::ListUnitServersRequest* req,
                                        pb::ListUnitServersResponse* resp,
                                        google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl; (void)req;
    set_ok(resp->mutable_status());
}

}  // namespace pl::minitable::master
