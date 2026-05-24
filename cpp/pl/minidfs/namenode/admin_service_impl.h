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

#pragma once

#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/minidfs/namenode/namespace_manager.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

namespace pl::minidfs {

// AdminServiceImpl — brpc service implementation for cluster administration.
//
// Provides read-only diagnostic and management RPCs for operators and CLI.
class AdminServiceImpl : public protocol::AdminService {
public:
    AdminServiceImpl(NamespaceManager* ns_mgr,
                     DataNodeManager* dn_mgr,
                     MetadataStore* metadata_store);
    ~AdminServiceImpl() override = default;

    void GetClusterInfo(google::protobuf::RpcController* controller,
                        const protocol::GetClusterInfoRequest* request,
                        protocol::GetClusterInfoResponse* response,
                        google::protobuf::Closure* done) override;

    void ListDataNodes(google::protobuf::RpcController* controller,
                       const protocol::ListDataNodesRequest* request,
                       protocol::ListDataNodesResponse* response,
                       google::protobuf::Closure* done) override;

    void GetDataNodeInfo(google::protobuf::RpcController* controller,
                         const protocol::GetDataNodeInfoRequest* request,
                         protocol::GetDataNodeInfoResponse* response,
                         google::protobuf::Closure* done) override;

    void GetInodeInfo(google::protobuf::RpcController* controller,
                      const protocol::GetInodeInfoRequest* request,
                      protocol::GetInodeInfoResponse* response,
                      google::protobuf::Closure* done) override;

    void GetFileBlocks(google::protobuf::RpcController* controller,
                       const protocol::GetFileBlocksRequest* request,
                       protocol::GetFileBlocksResponse* response,
                       google::protobuf::Closure* done) override;

    void GetBlockInfo(google::protobuf::RpcController* controller,
                      const protocol::GetBlockInfoRequest* request,
                      protocol::GetBlockInfoResponse* response,
                      google::protobuf::Closure* done) override;

private:
    static void fill_status(protocol::StatusProto* proto, uint32_t code, std::string_view msg = {});

    NamespaceManager* ns_mgr_;
    DataNodeManager* dn_mgr_;
    MetadataStore* metadata_store_;
};

} // namespace pl::minidfs
