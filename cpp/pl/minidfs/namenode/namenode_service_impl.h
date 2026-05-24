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

#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/minidfs/namenode/block_manager.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/minidfs/namenode/lease_manager.h"
#include "cpp/pl/minidfs/namenode/namespace_manager.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

namespace pl::minidfs {

// NameNodeServiceImpl — brpc service implementation for client-facing RPCs.
//
// Delegates to the internal managers (NamespaceManager, BlockManager, etc.)
// and translates between proto messages and internal types.

class NameNodeServiceImpl : public protocol::NameNodeService {
public:
    NameNodeServiceImpl(NamespaceManager* ns_mgr,
                        BlockManager* block_mgr,
                        LeaseManager* lease_mgr,
                        MetadataStore* metadata_store);
    ~NameNodeServiceImpl() override = default;

    void Mkdir(google::protobuf::RpcController* controller,
               const protocol::MkdirRequest* request,
               protocol::MkdirResponse* response,
               google::protobuf::Closure* done) override;

    void CreateFile(google::protobuf::RpcController* controller,
                    const protocol::CreateFileRequest* request,
                    protocol::CreateFileResponse* response,
                    google::protobuf::Closure* done) override;

    void CompleteFile(google::protobuf::RpcController* controller,
                      const protocol::CompleteFileRequest* request,
                      protocol::CompleteFileResponse* response,
                      google::protobuf::Closure* done) override;

    void GetFileStatus(google::protobuf::RpcController* controller,
                       const protocol::GetFileStatusRequest* request,
                       protocol::GetFileStatusResponse* response,
                       google::protobuf::Closure* done) override;

    void ListStatus(google::protobuf::RpcController* controller,
                    const protocol::ListStatusRequest* request,
                    protocol::ListStatusResponse* response,
                    google::protobuf::Closure* done) override;

    void Delete(google::protobuf::RpcController* controller,
                const protocol::DeleteRequest* request,
                protocol::DeleteResponse* response,
                google::protobuf::Closure* done) override;

    void Rename(google::protobuf::RpcController* controller,
                const protocol::RenameRequest* request,
                protocol::RenameResponse* response,
                google::protobuf::Closure* done) override;

    void AllocateBlock(google::protobuf::RpcController* controller,
                       const protocol::AllocateBlockRequest* request,
                       protocol::AllocateBlockResponse* response,
                       google::protobuf::Closure* done) override;

    void GetLocatedBlocks(google::protobuf::RpcController* controller,
                          const protocol::GetLocatedBlocksRequest* request,
                          protocol::GetLocatedBlocksResponse* response,
                          google::protobuf::Closure* done) override;

    void RenewLease(google::protobuf::RpcController* controller,
                    const protocol::RenewLeaseRequest* request,
                    protocol::RenewLeaseResponse* response,
                    google::protobuf::Closure* done) override;

private:
    static void fill_status(protocol::StatusProto* proto, uint32_t code, std::string_view msg = {});
    static void fill_file_status(protocol::FileStatusProto* proto, const FileStatus& fs);
    static void fill_located_block(protocol::LocatedBlockProto* proto, const LocatedBlock& lb);

    // Idempotency check: returns true if request_id was already processed (caller should return success)
    bool check_idempotent(const protocol::RequestHeader& header, protocol::StatusProto* status);

    // Write oplog (idempotency record)
    void write_oplog(std::string_view op_type,
                     uint64_t target_inode_id,
                     const protocol::RequestHeader& header);

    NamespaceManager* ns_mgr_;
    BlockManager* block_mgr_;
    LeaseManager* lease_mgr_;
    MetadataStore* metadata_store_;
};

// DataNodeProtocolServiceImpl — brpc service for DataNode-facing RPCs.

class DataNodeProtocolServiceImpl : public protocol::DataNodeProtocolService {
public:
    DataNodeProtocolServiceImpl(DataNodeManager* dn_mgr, BlockManager* block_mgr);
    ~DataNodeProtocolServiceImpl() override = default;

    void RegisterDataNode(google::protobuf::RpcController* controller,
                          const protocol::RegisterDataNodeRequest* request,
                          protocol::RegisterDataNodeResponse* response,
                          google::protobuf::Closure* done) override;

    void Heartbeat(google::protobuf::RpcController* controller,
                   const protocol::HeartbeatRequest* request,
                   protocol::HeartbeatResponse* response,
                   google::protobuf::Closure* done) override;

    void BlockReport(google::protobuf::RpcController* controller,
                     const protocol::BlockReportRequest* request,
                     protocol::BlockReportResponse* response,
                     google::protobuf::Closure* done) override;

    void CommitBlock(google::protobuf::RpcController* controller,
                     const protocol::CommitBlockRequest* request,
                     protocol::CommitBlockResponse* response,
                     google::protobuf::Closure* done) override;

private:
    static void fill_status(protocol::StatusProto* proto, uint32_t code, std::string_view msg = {});

    DataNodeManager* dn_mgr_;
    BlockManager* block_mgr_;
};

} // namespace pl::minidfs
