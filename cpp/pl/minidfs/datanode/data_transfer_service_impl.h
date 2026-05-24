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
// Created: 2026/05/10 18:30

#pragma once

#include "cpp/pl/minidfs/datanode/block_reporter.h"
#include "cpp/pl/minidfs/datanode/local_block_store.h"
#include "cpp/pl/minidfs/datanode/pipeline_receiver.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

namespace pl::minidfs {

// DataTransferServiceImpl — brpc service for DN-to-DN block transfers.
// Handles WriteBlock (pipeline write) and TransferBlock (full-block replication).
class DataTransferServiceImpl : public protocol::DataTransferService {
public:
    DataTransferServiceImpl(LocalBlockStore* store, BlockReporter* reporter);
    ~DataTransferServiceImpl() override = default;

    void WriteBlock(google::protobuf::RpcController* controller,
                    const protocol::WriteBlockRequest* request,
                    protocol::WriteBlockResponse* response,
                    google::protobuf::Closure* done) override;

    void ReadBlock(google::protobuf::RpcController* controller,
                   const protocol::ReadBlockRequest* request,
                   protocol::ReadBlockResponse* response,
                   google::protobuf::Closure* done) override;

    void TransferBlock(google::protobuf::RpcController* controller,
                       const protocol::TransferBlockRequest* request,
                       protocol::TransferBlockResponse* response,
                       google::protobuf::Closure* done) override;

private:
    static void fill_status(protocol::StatusProto* proto, uint32_t code, std::string_view msg = {});

    LocalBlockStore* store_;
    BlockReporter* reporter_;
};

} // namespace pl::minidfs
