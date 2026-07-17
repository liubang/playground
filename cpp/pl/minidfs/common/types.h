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
// Created: 2025/05/10 15:30

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cpp/pl/minidfs/common/constants.h"

namespace pl::minidfs {

// Inode
enum class InodeType : uint8_t {
    kDirectory = 1,
    kFile = 2,
};

enum class FileState : uint8_t {
    kNormal = 0,
    kUnderConstruction = 1,
    kDeleted = 2,
};

enum class FileAppendMode : uint8_t {
    kAppendable = 0,
    kImmutableAfterComplete = 1,
};

struct FileIdentity {
    uint64_t inode_id = 0;
    uint64_t content_generation = 0;
    uint64_t length = 0;
    uint32_t checksum = 0;
    bool checksum_valid = false;
};

struct Inode {
    uint64_t inode_id = 0;
    InodeType type = InodeType::kDirectory;
    uint64_t parent_id = 0;
    std::string name;

    std::string owner;
    std::string group;
    uint32_t permission = kDefaultPermission;

    uint64_t length = 0;
    uint32_t replication = kDefaultReplication;
    uint64_t block_size = kDefaultBlockSize;
    FileAppendMode file_append_mode = FileAppendMode::kAppendable;
    uint64_t content_generation = 0;
    uint32_t checksum = 0;
    bool checksum_valid = false;

    FileState state = FileState::kNormal;

    uint64_t ctime_ms = 0;
    uint64_t mtime_ms = 0;
    uint64_t version = 0;
};

// Block metadata (stored in MySQL, not to be confused with BlockHeader on disk)
enum class BlockState : uint8_t {
    kAllocating = 0,
    kCommitted = 1,
    kCorrupt = 2,
    kDeleted = 3,
};

struct BlockMeta {
    uint64_t block_id = 0;
    uint64_t inode_id = 0;
    uint32_t block_index = 0;
    uint64_t generation_stamp = 0;
    uint64_t length = 0;
    BlockState state = BlockState::kAllocating;
    uint32_t desired_replica = kDefaultReplication;
    uint64_t ctime_ms = 0;
    uint64_t mtime_ms = 0;
};

// Block Replica
enum class ReplicaState : uint8_t {
    kWriting = 0,
    kFinalized = 1,
    kCorrupt = 2,
    kStale = 3,
    kDeleting = 4,
    kDeleted = 5,
};

struct BlockReplica {
    uint64_t block_id = 0;
    uint64_t datanode_id = 0;
    uint64_t storage_id = 0;
    ReplicaState state = ReplicaState::kWriting;
    uint64_t length = 0;
    uint64_t generation_stamp = 0;
    uint64_t report_time_ms = 0;
};

// DataNode
enum class DataNodeState : uint8_t {
    kLive = 0,
    kStale = 1,
    kDead = 2,
    kDecommissioning = 3,
    kDecommissioned = 4,
};

struct DataNodeInfo {
    uint64_t datanode_id = 0;
    std::string uuid;
    std::string hostname;
    std::string ip;
    uint32_t rpc_port = 0;
    uint32_t data_port = 0;
    std::string rack = "/default-rack";
    DataNodeState state = DataNodeState::kLive;
    uint64_t capacity_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t last_heartbeat_ms = 0;
};

// Lease
enum class LeaseState : uint8_t {
    kActive = 0,
    kClosed = 1,
};

struct Lease {
    uint64_t lease_id = 0;
    uint64_t inode_id = 0;
    std::string client_id;
    LeaseState state = LeaseState::kActive;
    uint64_t expire_time_ms = 0;
    uint64_t ctime_ms = 0;
    uint64_t mtime_ms = 0;
};

struct OplogEntry {
    std::string op_type;
    uint64_t target_inode_id = 0;
    std::string request_id;
    std::string payload_json;
};

// Located Block (used in client-facing responses)
struct DataNodeEndpoint {
    uint64_t datanode_id = 0;
    std::string host;
    uint32_t data_port = 0;
};

struct BlockToken {
    uint64_t block_id = 0;
    uint64_t generation_stamp = 0;
    uint64_t inode_id = 0;
    uint32_t block_index = 0;
    uint32_t permissions = 0;
    uint64_t expires_at_ms = 0;
    std::string signature;
    std::optional<FileIdentity> file_identity;
};

struct LocatedBlock {
    uint64_t block_id = 0;
    uint64_t generation_stamp = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    std::vector<DataNodeEndpoint> locations;
    BlockToken block_token;
};

// File Status (used in list/stat responses)
struct FileStatus {
    uint64_t inode_id = 0;
    std::string path;
    bool is_dir = false;
    uint64_t length = 0;
    uint32_t replication = 0;
    uint64_t block_size = 0;
    uint64_t mtime_ms = 0;
    std::string owner;
    std::string group;
    uint32_t permission = 0;
    FileAppendMode file_append_mode = FileAppendMode::kAppendable;
    FileIdentity published_identity;
};

} // namespace pl::minidfs
