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

#include "cpp/pl/minidfs/namenode/admin_service_impl.h"

#include <brpc/closure_guard.h>

#include "cpp/pl/minidfs/common/error_code.h"

namespace pl::minidfs {

AdminServiceImpl::AdminServiceImpl(NamespaceManager* ns_mgr,
                                   DataNodeManager* dn_mgr,
                                   MetadataStore* metadata_store)
    : ns_mgr_(ns_mgr), dn_mgr_(dn_mgr), metadata_store_(metadata_store) {}

void AdminServiceImpl::fill_status(protocol::StatusProto* proto,
                                   uint32_t code,
                                   std::string_view msg) {
    proto->set_code(code);
    if (!msg.empty()) {
        proto->set_message(std::string(msg));
    }
}

void AdminServiceImpl::GetClusterInfo(google::protobuf::RpcController* /*controller*/,
                                      const protocol::GetClusterInfoRequest* /*request*/,
                                      protocol::GetClusterInfoResponse* response,
                                      google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    auto all_dns = dn_mgr_->get_all_datanodes();
    if (all_dns.hasError()) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kInternalError),
                    all_dns.error().describe());
        return;
    }

    uint64_t total_capacity = 0;
    uint64_t total_used = 0;
    uint64_t total_free = 0;
    uint32_t live_count = 0;
    uint32_t dead_count = 0;

    for (const auto& dn : all_dns.value()) {
        total_capacity += dn.capacity_bytes;
        total_used += dn.used_bytes;
        total_free += dn.free_bytes;
        if (dn.state == DataNodeState::kLive) {
            ++live_count;
        } else if (dn.state == DataNodeState::kDead) {
            ++dead_count;
        }
    }

    // Count blocks and files/directories via metadata store
    auto committed_blocks = metadata_store_->get_blocks_by_state(BlockState::kCommitted);
    auto allocating_blocks = metadata_store_->get_blocks_by_state(BlockState::kAllocating);

    uint64_t total_blocks = 0;
    if (committed_blocks.hasValue()) {
        total_blocks += committed_blocks.value().size();
    }
    if (allocating_blocks.hasValue()) {
        total_blocks += allocating_blocks.value().size();
    }

    // Count files and directories via root listing (approximate: count immediate namespace)
    auto root_children = metadata_store_->list_children(1); // root inode_id=1
    uint64_t file_count = 0;
    uint64_t dir_count = 1; // root itself
    if (root_children.hasValue()) {
        for (const auto& child : root_children.value()) {
            if (child.type == InodeType::kDirectory) {
                ++dir_count;
            } else {
                ++file_count;
            }
        }
    }

    // Count under-replicated blocks
    uint32_t under_replicated = 0;
    if (committed_blocks.hasValue()) {
        for (const auto& block : committed_blocks.value()) {
            auto replicas = metadata_store_->get_replicas(block.block_id);
            if (replicas.hasValue()) {
                uint32_t finalized = 0;
                for (const auto& r : replicas.value()) {
                    if (r.state == ReplicaState::kFinalized) {
                        ++finalized;
                    }
                }
                if (finalized < block.desired_replica) {
                    ++under_replicated;
                }
            }
        }
    }

    fill_status(response->mutable_status(), 0);
    response->set_total_capacity_bytes(total_capacity);
    response->set_used_bytes(total_used);
    response->set_free_bytes(total_free);
    response->set_live_datanodes(live_count);
    response->set_dead_datanodes(dead_count);
    response->set_total_blocks(total_blocks);
    response->set_total_files(file_count);
    response->set_total_directories(dir_count);
    response->set_under_replicated_blocks(under_replicated);
}

void AdminServiceImpl::ListDataNodes(google::protobuf::RpcController* /*controller*/,
                                     const protocol::ListDataNodesRequest* request,
                                     protocol::ListDataNodesResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    auto all_dns = dn_mgr_->get_all_datanodes();
    if (all_dns.hasError()) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kInternalError),
                    all_dns.error().describe());
        return;
    }

    auto state_to_string = [](DataNodeState state) -> std::string {
        switch (state) {
            case DataNodeState::kLive:
                return "live";
            case DataNodeState::kStale:
                return "stale";
            case DataNodeState::kDead:
                return "dead";
            case DataNodeState::kDecommissioning:
                return "decommissioning";
            case DataNodeState::kDecommissioned:
                return "decommissioned";
        }
        return "unknown";
    };

    for (const auto& dn : all_dns.value()) {
        if (!request->include_dead() && dn.state == DataNodeState::kDead) {
            continue;
        }

        auto* info = response->add_datanodes();
        info->set_datanode_id(dn.datanode_id);
        info->set_uuid(dn.uuid);
        info->set_hostname(dn.hostname);
        info->set_ip(dn.ip);
        info->set_rpc_port(dn.rpc_port);
        info->set_data_port(dn.data_port);
        info->set_rack(dn.rack);
        info->set_state(state_to_string(dn.state));
        info->set_capacity_bytes(dn.capacity_bytes);
        info->set_used_bytes(dn.used_bytes);
        info->set_free_bytes(dn.free_bytes);
        info->set_last_heartbeat_ms(dn.last_heartbeat_ms);

        // Count blocks on this datanode
        auto replicas = metadata_store_->get_replicas_by_datanode(dn.datanode_id);
        if (replicas.hasValue()) {
            info->set_block_count(static_cast<uint32_t>(replicas.value().size()));
        }
    }

    fill_status(response->mutable_status(), 0);
}

void AdminServiceImpl::GetDataNodeInfo(google::protobuf::RpcController* /*controller*/,
                                       const protocol::GetDataNodeInfoRequest* request,
                                       protocol::GetDataNodeInfoResponse* response,
                                       google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    DataNodeInfo dn_info;
    bool found = false;

    if (request->datanode_id() != 0) {
        auto result = dn_mgr_->get_datanode(request->datanode_id());
        if (result.hasValue()) {
            dn_info = result.value();
            found = true;
        }
    } else if (!request->uuid().empty()) {
        auto result = metadata_store_->get_datanode_by_uuid(request->uuid());
        if (result.hasValue() && result.value().has_value()) {
            dn_info = result.value().value();
            found = true;
        }
    }

    if (!found) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kNotFound),
                    "DataNode not found");
        return;
    }

    auto state_to_string = [](DataNodeState state) -> std::string {
        switch (state) {
            case DataNodeState::kLive:
                return "live";
            case DataNodeState::kStale:
                return "stale";
            case DataNodeState::kDead:
                return "dead";
            case DataNodeState::kDecommissioning:
                return "decommissioning";
            case DataNodeState::kDecommissioned:
                return "decommissioned";
        }
        return "unknown";
    };

    auto* info = response->mutable_datanode();
    info->set_datanode_id(dn_info.datanode_id);
    info->set_uuid(dn_info.uuid);
    info->set_hostname(dn_info.hostname);
    info->set_ip(dn_info.ip);
    info->set_rpc_port(dn_info.rpc_port);
    info->set_data_port(dn_info.data_port);
    info->set_rack(dn_info.rack);
    info->set_state(state_to_string(dn_info.state));
    info->set_capacity_bytes(dn_info.capacity_bytes);
    info->set_used_bytes(dn_info.used_bytes);
    info->set_free_bytes(dn_info.free_bytes);
    info->set_last_heartbeat_ms(dn_info.last_heartbeat_ms);

    // List blocks on this datanode
    auto replicas = metadata_store_->get_replicas_by_datanode(dn_info.datanode_id);
    if (replicas.hasValue()) {
        info->set_block_count(static_cast<uint32_t>(replicas.value().size()));
        for (const auto& r : replicas.value()) {
            auto* block_info = response->add_blocks();
            block_info->set_block_id(r.block_id);
            block_info->set_generation_stamp(r.generation_stamp);
            block_info->set_length(r.length);
        }
    }

    fill_status(response->mutable_status(), 0);
}

void AdminServiceImpl::GetInodeInfo(google::protobuf::RpcController* /*controller*/,
                                    const protocol::GetInodeInfoRequest* request,
                                    protocol::GetInodeInfoResponse* response,
                                    google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    Inode inode;
    bool found = false;

    if (request->inode_id() != 0) {
        auto result = metadata_store_->get_inode(request->inode_id());
        if (result.hasValue()) {
            inode = result.value();
            found = true;
        }
    } else if (!request->path().empty()) {
        auto result = ns_mgr_->resolve_path(request->path());
        if (result.hasValue()) {
            inode = result.value();
            found = true;
        }
    }

    if (!found) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kFileNotFound),
                    "Inode not found");
        return;
    }

    auto* info = response->mutable_inode();
    info->set_inode_id(inode.inode_id);
    info->set_type(inode.type == InodeType::kDirectory ? "directory" : "file");
    info->set_parent_id(inode.parent_id);
    info->set_name(inode.name);
    info->set_owner(inode.owner);
    info->set_group(inode.group);
    info->set_permission(inode.permission);
    info->set_length(inode.length);
    info->set_replication(inode.replication);
    info->set_block_size(inode.block_size);

    switch (inode.state) {
        case FileState::kNormal:
            info->set_state("normal");
            break;
        case FileState::kUnderConstruction:
            info->set_state("under_construction");
            break;
        case FileState::kDeleted:
            info->set_state("deleted");
            break;
    }

    info->set_ctime_ms(inode.ctime_ms);
    info->set_mtime_ms(inode.mtime_ms);

    // Block count for files
    if (inode.type == InodeType::kFile) {
        auto blocks = metadata_store_->get_blocks_by_inode(inode.inode_id);
        if (blocks.hasValue()) {
            info->set_block_count(static_cast<uint32_t>(blocks.value().size()));
        }
    }

    // Child count for directories
    if (inode.type == InodeType::kDirectory) {
        auto children = metadata_store_->list_children(inode.inode_id);
        if (children.hasValue()) {
            info->set_child_count(static_cast<uint32_t>(children.value().size()));
        }
    }

    fill_status(response->mutable_status(), 0);
}

void AdminServiceImpl::GetFileBlocks(google::protobuf::RpcController* /*controller*/,
                                     const protocol::GetFileBlocksRequest* request,
                                     protocol::GetFileBlocksResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    Inode inode;
    bool found = false;

    if (request->inode_id() != 0) {
        auto result = metadata_store_->get_inode(request->inode_id());
        if (result.hasValue()) {
            inode = result.value();
            found = true;
        }
    } else if (!request->path().empty()) {
        auto result = ns_mgr_->resolve_path(request->path());
        if (result.hasValue()) {
            inode = result.value();
            found = true;
        }
    }

    if (!found) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kFileNotFound),
                    "File not found");
        return;
    }

    if (inode.type != InodeType::kFile) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kIsDirectory),
                    "Path is not a file");
        return;
    }

    // Fill file info
    auto* file_info = response->mutable_file_info();
    file_info->set_inode_id(inode.inode_id);
    file_info->set_type("file");
    file_info->set_name(inode.name);
    file_info->set_owner(inode.owner);
    file_info->set_length(inode.length);
    file_info->set_replication(inode.replication);
    file_info->set_block_size(inode.block_size);

    // List blocks
    auto blocks = metadata_store_->get_blocks_by_inode(inode.inode_id);
    if (blocks.hasError()) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kInternalError),
                    blocks.error().describe());
        return;
    }

    auto block_state_name = [](BlockState state) -> std::string {
        switch (state) {
            case BlockState::kAllocating:
                return "allocating";
            case BlockState::kCommitted:
                return "committed";
            case BlockState::kCorrupt:
                return "corrupt";
            case BlockState::kDeleted:
                return "deleted";
        }
        return "unknown";
    };

    for (const auto& block : blocks.value()) {
        auto* bi = response->add_blocks();
        bi->set_block_id(block.block_id);
        bi->set_block_index(block.block_index);
        bi->set_generation_stamp(block.generation_stamp);
        bi->set_length(block.length);
        bi->set_state(block_state_name(block.state));
        bi->set_desired_replicas(block.desired_replica);

        // Get replicas for this block
        auto replicas = metadata_store_->get_replicas(block.block_id);
        if (replicas.hasValue()) {
            uint32_t actual = 0;
            for (const auto& r : replicas.value()) {
                if (r.state == ReplicaState::kFinalized) {
                    ++actual;
                }
                auto* loc = bi->add_locations();
                loc->set_datanode_id(r.datanode_id);
                // Optionally fill host/port by looking up the datanode
                auto dn = metadata_store_->get_datanode(r.datanode_id);
                if (dn.hasValue()) {
                    loc->set_host(dn.value().hostname);
                    loc->set_data_port(dn.value().data_port);
                }
            }
            bi->set_actual_replicas(actual);
        }
    }

    fill_status(response->mutable_status(), 0);
}

void AdminServiceImpl::GetBlockInfo(google::protobuf::RpcController* /*controller*/,
                                    const protocol::GetBlockInfoRequest* request,
                                    protocol::GetBlockInfoResponse* response,
                                    google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    auto block_result = metadata_store_->get_block(request->block_id());
    if (block_result.hasError()) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kBlockNotFound),
                    "Block not found");
        return;
    }

    const auto& block = block_result.value();

    auto block_state_name = [](BlockState state) -> std::string {
        switch (state) {
            case BlockState::kAllocating:
                return "allocating";
            case BlockState::kCommitted:
                return "committed";
            case BlockState::kCorrupt:
                return "corrupt";
            case BlockState::kDeleted:
                return "deleted";
        }
        return "unknown";
    };

    auto replica_state_name = [](ReplicaState state) -> std::string {
        switch (state) {
            case ReplicaState::kWriting:
                return "writing";
            case ReplicaState::kFinalized:
                return "finalized";
            case ReplicaState::kCorrupt:
                return "corrupt";
            case ReplicaState::kStale:
                return "stale";
            case ReplicaState::kDeleting:
                return "deleting";
            case ReplicaState::kDeleted:
                return "deleted";
        }
        return "unknown";
    };

    response->set_block_id(block.block_id);
    response->set_inode_id(block.inode_id);
    response->set_block_index(block.block_index);
    response->set_generation_stamp(block.generation_stamp);
    response->set_length(block.length);
    response->set_state(block_state_name(block.state));
    response->set_desired_replicas(block.desired_replica);

    // Get replicas
    auto replicas = metadata_store_->get_replicas(block.block_id);
    if (replicas.hasValue()) {
        for (const auto& r : replicas.value()) {
            auto* ri = response->add_replicas();
            ri->set_datanode_id(r.datanode_id);
            ri->set_state(replica_state_name(r.state));
            ri->set_length(r.length);
            ri->set_generation_stamp(r.generation_stamp);
            ri->set_report_time_ms(r.report_time_ms);

            auto dn = metadata_store_->get_datanode(r.datanode_id);
            if (dn.hasValue()) {
                ri->set_hostname(dn.value().hostname);
            }
        }
    }

    fill_status(response->mutable_status(), 0);
}

} // namespace pl::minidfs
