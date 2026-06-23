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

#include <brpc/channel.h>
#include <brpc/server.h>

#include "cpp/pl/minitable/master/master_sm.h"
#include "cpp/pl/minitable/proto/admin.pb.h"
#include "cpp/pl/minitable/proto/master.pb.h"
#include "cpp/pl/minitable/proto/unit.pb.h"
#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

// ---------------------------------------------------------------------------
// MasterServiceImpl — 实现三个 brpc Service
// ---------------------------------------------------------------------------
// brpc 线程只做: 反序列化, 参数校验, dispatch → 模块队列

class MasterServiceImpl final : public proto::MasterService,
                                public proto::AdminService,
                                public proto::UnitService {
public:
    explicit MasterServiceImpl(MasterSM* sm) : sm_(sm) {}

    // ---- MasterService (client-facing) ----

    void CreateTable(google::protobuf::RpcController* cntl,
                     const proto::CreateTableRequest* req,
                     proto::CreateTableResponse* resp,
                     google::protobuf::Closure* done) override;

    void DropTable(google::protobuf::RpcController* cntl,
                   const proto::DropTableRequest* req,
                   proto::DropTableResponse* resp,
                   google::protobuf::Closure* done) override;

    void GetSlice(google::protobuf::RpcController* cntl,
                  const proto::GetSliceRequest* req,
                  proto::GetSliceResponse* resp,
                  google::protobuf::Closure* done) override;

    // ---- AdminService (management) ----

    void CreateRegion(google::protobuf::RpcController* cntl,
                      const proto::CreateRegionRequest* req,
                      proto::CreateRegionResponse* resp,
                      google::protobuf::Closure* done) override;

    void DropRegion(google::protobuf::RpcController* cntl,
                    const proto::DropRegionRequest* req,
                    proto::DropRegionResponse* resp,
                    google::protobuf::Closure* done) override;

    // ---- UnitService (Master↔UnitServer internal) ----

    void Heartbeat(google::protobuf::RpcController* cntl,
                   const proto::HeartbeatRequest* req,
                   proto::HeartbeatResponse* resp,
                   google::protobuf::Closure* done) override;

    void RegisterUnitServer(google::protobuf::RpcController* cntl,
                            const proto::RegisterUnitServerRequest* req,
                            proto::RegisterUnitServerResponse* resp,
                            google::protobuf::Closure* done) override;

private:
    MasterSM* sm_; // 不拥有
};

} // namespace pl::minitable::master
