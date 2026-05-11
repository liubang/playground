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

#include "cpp/pl/minidfs/namenode/namenode_service_impl.h"

#include "cpp/pl/minidfs/common/error_code.h"
#include <brpc/closure_guard.h>

namespace pl::minidfs {

// ============================================================================
// Helper utilities
// ============================================================================

void NameNodeServiceImpl::fill_status(protocol::StatusProto* proto,
                                      uint32_t code,
                                      std::string_view msg) {
    proto->set_code(code);
    if (!msg.empty()) {
        proto->set_message(std::string(msg));
    }
}

void NameNodeServiceImpl::fill_file_status(protocol::FileStatusProto* proto, const FileStatus& fs) {
    proto->set_inode_id(fs.inode_id);
    proto->set_path(fs.path);
    proto->set_is_dir(fs.is_dir);
    proto->set_length(fs.length);
    proto->set_replication(fs.replication);
    proto->set_block_size(fs.block_size);
    proto->set_mtime_ms(fs.mtime_ms);
    proto->set_owner(fs.owner);
    proto->set_group(fs.group);
    proto->set_permission(fs.permission);
}

void NameNodeServiceImpl::fill_located_block(protocol::LocatedBlockProto* proto,
                                             const LocatedBlock& lb) {
    proto->set_block_id(lb.block_id);
    proto->set_generation_stamp(lb.generation_stamp);
    proto->set_offset(lb.offset);
    proto->set_length(lb.length);
    for (const auto& loc : lb.locations) {
        auto* ep = proto->add_locations();
        ep->set_datanode_id(loc.datanode_id);
        ep->set_host(loc.host);
        ep->set_data_port(loc.data_port);
    }
}

// ============================================================================
// NameNodeServiceImpl construction
// ============================================================================

NameNodeServiceImpl::NameNodeServiceImpl(NamespaceManager* ns_mgr,
                                         BlockManager* block_mgr,
                                         LeaseManager* lease_mgr,
                                         MetadataStore* metadata_store)
    : ns_mgr_(ns_mgr), block_mgr_(block_mgr), lease_mgr_(lease_mgr),
      metadata_store_(metadata_store) {}

bool NameNodeServiceImpl::check_idempotent(const protocol::RequestHeader& header,
                                           protocol::StatusProto* status) {
    if (header.request_id().empty()) {
        return false;
    }
    auto result = metadata_store_->check_request_id(header.request_id());
    if (result.hasError()) {
        return false;
    }
    if (result.value()) {
        // 已处理过，直接返回成功
        fill_status(status, 0);
        return true;
    }
    return false;
}

void NameNodeServiceImpl::write_oplog(std::string_view op_type,
                                      uint64_t target_inode_id,
                                      const protocol::RequestHeader& header) {
    if (header.request_id().empty()) {
        return;
    }
    // best-effort: 写入失败不影响主流程
    (void)metadata_store_->write_oplog(op_type, target_inode_id,
                                       header.request_id(), "{}");
}

// ============================================================================
// Namespace Operations
// ============================================================================

void NameNodeServiceImpl::Mkdir(google::protobuf::RpcController* /*controller*/,
                                const protocol::MkdirRequest* request,
                                protocol::MkdirResponse* response,
                                google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    if (request->has_header() && check_idempotent(request->header(), response->mutable_status())) {
        return;
    }

    auto result = ns_mgr_->mkdir(request->path(), request->owner(), request->group(),
                                 request->permission(), /*create_parent=*/true);
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    if (request->has_header()) {
        write_oplog("mkdir", result.value().inode_id, request->header());
    }
    fill_status(response->mutable_status(), 0);
    response->set_inode_id(result.value().inode_id);
}

void NameNodeServiceImpl::CreateFile(google::protobuf::RpcController* /*controller*/,
                                     const protocol::CreateFileRequest* request,
                                     protocol::CreateFileResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    if (request->has_header() && check_idempotent(request->header(), response->mutable_status())) {
        return;
    }

    uint32_t replication =
        request->replication() > 0 ? request->replication() : kDefaultReplication;
    uint64_t block_size = request->block_size() > 0 ? request->block_size() : kDefaultBlockSize;

    auto result = ns_mgr_->create_file(request->path(), request->owner(), request->group(),
                                       request->permission(), replication, block_size);
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    // Acquire lease for the client
    auto lease_result = lease_mgr_->acquire_lease(result.value().inode_id, request->client_id());
    if (lease_result.hasError()) {
        fill_status(response->mutable_status(), lease_result.error().code(),
                    lease_result.error().message());
        return;
    }

    if (request->has_header()) {
        write_oplog("create_file", result.value().inode_id, request->header());
    }
    fill_status(response->mutable_status(), 0);
    response->set_inode_id(result.value().inode_id);
    response->set_lease_id(lease_result.value().lease_id);
}

void NameNodeServiceImpl::CompleteFile(google::protobuf::RpcController* /*controller*/,
                                       const protocol::CompleteFileRequest* request,
                                       protocol::CompleteFileResponse* response,
                                       google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    // Get located blocks to compute final file length
    auto blocks_result = block_mgr_->get_located_blocks(request->inode_id());
    if (blocks_result.hasError()) {
        fill_status(response->mutable_status(), blocks_result.error().code(),
                    blocks_result.error().message());
        return;
    }

    uint64_t total_length = 0;
    for (const auto& b : blocks_result.value()) {
        total_length += b.length;
    }

    // Complete the file (transition state)
    auto complete_result = ns_mgr_->complete_file(request->inode_id(), total_length);
    if (complete_result.hasError()) {
        fill_status(response->mutable_status(), complete_result.error().code(),
                    complete_result.error().message());
        return;
    }

    // Release lease
    lease_mgr_->release_lease(request->inode_id(), request->client_id());

    fill_status(response->mutable_status(), 0);
}

void NameNodeServiceImpl::GetFileStatus(google::protobuf::RpcController* /*controller*/,
                                        const protocol::GetFileStatusRequest* request,
                                        protocol::GetFileStatusResponse* response,
                                        google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = ns_mgr_->get_file_status(request->path());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    fill_file_status(response->mutable_file_status(), result.value());
}

void NameNodeServiceImpl::ListStatus(google::protobuf::RpcController* /*controller*/,
                                     const protocol::ListStatusRequest* request,
                                     protocol::ListStatusResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = ns_mgr_->list_status(request->path());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    for (const auto& entry : result.value()) {
        fill_file_status(response->add_entries(), entry);
    }
}

void NameNodeServiceImpl::Delete(google::protobuf::RpcController* /*controller*/,
                                 const protocol::DeleteRequest* request,
                                 protocol::DeleteResponse* response,
                                 google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    if (request->has_header() && check_idempotent(request->header(), response->mutable_status())) {
        return;
    }

    auto result = ns_mgr_->remove(request->path(), request->recursive());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    if (request->has_header()) {
        write_oplog("delete", result.value().inode_id, request->header());
    }
    fill_status(response->mutable_status(), 0);
}

void NameNodeServiceImpl::Rename(google::protobuf::RpcController* /*controller*/,
                                 const protocol::RenameRequest* request,
                                 protocol::RenameResponse* response,
                                 google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    if (request->has_header() && check_idempotent(request->header(), response->mutable_status())) {
        return;
    }

    auto result = ns_mgr_->rename(request->src(), request->dst());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    if (request->has_header()) {
        write_oplog("rename", 0, request->header());
    }
    fill_status(response->mutable_status(), 0);
}

// ============================================================================
// Block Operations
// ============================================================================

void NameNodeServiceImpl::AllocateBlock(google::protobuf::RpcController* /*controller*/,
                                        const protocol::AllocateBlockRequest* request,
                                        protocol::AllocateBlockResponse* response,
                                        google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    if (request->has_header() && check_idempotent(request->header(), response->mutable_status())) {
        return;
    }

    uint32_t replication =
        request->replication() > 0 ? request->replication() : kDefaultReplication;
    auto result =
        block_mgr_->allocate_block(request->inode_id(), request->block_index(), replication);
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    if (request->has_header()) {
        write_oplog("allocate_block", request->inode_id(), request->header());
    }
    fill_status(response->mutable_status(), 0);
    fill_located_block(response->mutable_block(), result.value());
}

void NameNodeServiceImpl::GetLocatedBlocks(google::protobuf::RpcController* /*controller*/,
                                           const protocol::GetLocatedBlocksRequest* request,
                                           protocol::GetLocatedBlocksResponse* response,
                                           google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = block_mgr_->get_located_blocks(request->inode_id());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    for (const auto& lb : result.value()) {
        fill_located_block(response->add_blocks(), lb);
    }
}

// ============================================================================
// Lease Operations
// ============================================================================

void NameNodeServiceImpl::RenewLease(google::protobuf::RpcController* /*controller*/,
                                     const protocol::RenewLeaseRequest* request,
                                     protocol::RenewLeaseResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = lease_mgr_->renew_lease(request->inode_id(), request->client_id());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }
    fill_status(response->mutable_status(), 0);
}

// ============================================================================
// DataNodeProtocolServiceImpl
// ============================================================================

void DataNodeProtocolServiceImpl::fill_status(protocol::StatusProto* proto,
                                              uint32_t code,
                                              std::string_view msg) {
    proto->set_code(code);
    if (!msg.empty()) {
        proto->set_message(std::string(msg));
    }
}

DataNodeProtocolServiceImpl::DataNodeProtocolServiceImpl(DataNodeManager* dn_mgr,
                                                         BlockManager* block_mgr)
    : dn_mgr_(dn_mgr), block_mgr_(block_mgr) {}

void DataNodeProtocolServiceImpl::RegisterDataNode(google::protobuf::RpcController* /*controller*/,
                                                   const protocol::RegisterDataNodeRequest* request,
                                                   protocol::RegisterDataNodeResponse* response,
                                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = dn_mgr_->register_datanode(request->uuid(), request->hostname(), request->ip(),
                                             request->rpc_port(), request->data_port(),
                                             request->rack(), request->capacity_bytes());

    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    response->set_datanode_id(result.value());
}

void DataNodeProtocolServiceImpl::Heartbeat(google::protobuf::RpcController* /*controller*/,
                                            const protocol::HeartbeatRequest* request,
                                            protocol::HeartbeatResponse* response,
                                            google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = dn_mgr_->handle_heartbeat(request->datanode_id(), request->capacity_bytes(),
                                            request->used_bytes(), request->free_bytes());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    // Commands are generated separately by ReplicationManager scans;
    // for now return empty commands. In production, a command queue
    // per-datanode would be drained here.
}

void DataNodeProtocolServiceImpl::BlockReport(google::protobuf::RpcController* /*controller*/,
                                              const protocol::BlockReportRequest* request,
                                              protocol::BlockReportResponse* response,
                                              google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    // For each reported block, we could verify against MetadataStore.
    // For now, accept the report and return empty blocks_to_delete.
    // Full reconciliation logic will be added in a later iteration.
    (void)request;
    fill_status(response->mutable_status(), 0);
}

void DataNodeProtocolServiceImpl::CommitBlock(google::protobuf::RpcController* /*controller*/,
                                              const protocol::CommitBlockRequest* request,
                                              protocol::CommitBlockResponse* response,
                                              google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = block_mgr_->commit_block(request->block_id(), request->length(),
                                           request->generation_stamp());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }
    fill_status(response->mutable_status(), 0);
}

} // namespace pl::minidfs
