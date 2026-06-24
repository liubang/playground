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

#pragma once

#include <brpc/channel.h>
#include <brpc/server.h>

#include "cpp/pl/minitable/master/master_sm.h"
#include "cpp/pl/minitable/proto/common.pb.h"
#include "cpp/pl/minitable/proto/master.pb.h"

namespace pl::minitable {

namespace pb = pl::minitable::proto;

namespace master {

// ---------------------------------------------------------------------------
// MasterServiceImpl — 客户端 DDL + 分片查询
// ---------------------------------------------------------------------------

class MasterServiceImpl final : public pb::MasterService {
public:
    explicit MasterServiceImpl(MasterSM* sm) : sm_(sm) {}

    void CreateTable(google::protobuf::RpcController* cntl,
                     const pb::CreateTableRequest* req,
                     pb::CreateTableResponse* resp,
                     google::protobuf::Closure* done) override;

    void UpdateTable(google::protobuf::RpcController* cntl,
                     const pb::UpdateTableRequest* req,
                     pb::UpdateTableResponse* resp,
                     google::protobuf::Closure* done) override;

    void DropTable(google::protobuf::RpcController* cntl,
                   const pb::DropTableRequest* req,
                   pb::DropTableResponse* resp,
                   google::protobuf::Closure* done) override;

    void GetTable(google::protobuf::RpcController* cntl,
                  const pb::GetTableRequest* req,
                  pb::GetTableResponse* resp,
                  google::protobuf::Closure* done) override;

    void ListTables(google::protobuf::RpcController* cntl,
                    const pb::ListTablesRequest* req,
                    pb::ListTablesResponse* resp,
                    google::protobuf::Closure* done) override;

    void GetSlice(google::protobuf::RpcController* cntl,
                  const pb::GetSliceRequest* req,
                  pb::GetSliceResponse* resp,
                  google::protobuf::Closure* done) override;

private:
    MasterSM* sm_;
};

}  // namespace master
}  // namespace pl::minitable
