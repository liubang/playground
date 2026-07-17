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

#pragma once

#include <brpc/channel.h>
#include <brpc/server.h>

#include "cpp/pl/minitable/master/master_sm.h"
#include "cpp/pl/minitable/proto/admin.pb.h"
#include "cpp/pl/minitable/proto/common.pb.h"

namespace pl::minitable {

namespace pb = pl::minitable::proto;

namespace master {

// ---------------------------------------------------------------------------
// AdminServiceImpl — 集群管理接口 (Region / UnitServer)
// ---------------------------------------------------------------------------

class AdminServiceImpl final : public pb::AdminService {
public:
    explicit AdminServiceImpl(MasterSM* sm) : sm_(sm) {}

    void CreateRegion(google::protobuf::RpcController* cntl,
                      const pb::CreateRegionRequest* req,
                      pb::CreateRegionResponse* resp,
                      google::protobuf::Closure* done) override;

    void DropRegion(google::protobuf::RpcController* cntl,
                    const pb::DropRegionRequest* req,
                    pb::DropRegionResponse* resp,
                    google::protobuf::Closure* done) override;

    void GetRegion(google::protobuf::RpcController* cntl,
                   const pb::GetRegionRequest* req,
                   pb::GetRegionResponse* resp,
                   google::protobuf::Closure* done) override;

    void ListRegions(google::protobuf::RpcController* cntl,
                     const pb::ListRegionsRequest* req,
                     pb::ListRegionsResponse* resp,
                     google::protobuf::Closure* done) override;

    void ListUnitServers(google::protobuf::RpcController* cntl,
                         const pb::ListUnitServersRequest* req,
                         pb::ListUnitServersResponse* resp,
                         google::protobuf::Closure* done) override;

private:
    MasterSM* sm_;
};

} // namespace master
} // namespace pl::minitable
