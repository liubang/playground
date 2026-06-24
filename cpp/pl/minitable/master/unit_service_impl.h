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
#include "cpp/pl/minitable/proto/common.pb.h"
#include "cpp/pl/minitable/proto/unit.pb.h"

namespace pl::minitable {

namespace pb = pl::minitable::proto;

namespace master {

// ---------------------------------------------------------------------------
// UnitServiceImpl — Master ↔ UnitServer 内部通信
// ---------------------------------------------------------------------------

class UnitServiceImpl final : public pb::UnitService {
public:
    explicit UnitServiceImpl(MasterSM* sm) : sm_(sm) {}

    void Heartbeat(google::protobuf::RpcController* cntl,
                   const pb::HeartbeatRequest* req,
                   pb::HeartbeatResponse* resp,
                   google::protobuf::Closure* done) override;

    void RegisterUnitServer(google::protobuf::RpcController* cntl,
                            const pb::RegisterUnitServerRequest* req,
                            pb::RegisterUnitServerResponse* resp,
                            google::protobuf::Closure* done) override;

private:
    MasterSM* sm_;
};

}  // namespace master
}  // namespace pl::minitable
