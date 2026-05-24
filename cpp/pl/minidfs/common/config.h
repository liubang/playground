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

#include <cstdint>
#include <string>
#include <vector>

#include "cpp/pl/status/result.h"

namespace pl::minidfs {

struct ServerConfig {
    std::string host = "0.0.0.0";
    uint32_t port = 9000;
};

struct MySQLConfigYaml {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "minidfs";
    std::string password = "minidfs";
    std::string database = "minidfs";
    uint32_t pool_size = 16;
};

struct FilesystemConfig {
    uint32_t default_replication = 3;
    uint64_t default_block_size = 134217728; // 128MB
    uint64_t default_chunk_size = 1048576;   // 1MB
    uint32_t min_write_replica = 2;
};

struct HeartbeatConfig {
    uint64_t stale_timeout_ms = 30000;
    uint64_t dead_timeout_ms = 600000;
    uint64_t interval_ms = 3000;
};

struct ReplicationConfig {
    uint64_t scan_interval_ms = 30000;
    uint32_t max_replication_tasks_per_round = 100;
};

struct NameNodeConfig {
    ServerConfig server;
    MySQLConfigYaml mysql;
    FilesystemConfig filesystem;
    HeartbeatConfig heartbeat;
    ReplicationConfig replication;
};

struct StorageConfig {
    std::vector<std::string> data_dirs;
    uint64_t reserved_bytes = 1073741824; // 1GB
};

struct BlockReportConfig {
    uint64_t interval_ms = 600000;
};

struct DataNodeServerConfig {
    std::string host = "0.0.0.0";
    uint32_t data_port = 9100;
    uint32_t rpc_port = 9101;
};

struct NameNodeEndpointConfig {
    std::string host = "127.0.0.1";
    uint32_t port = 9000;
};

struct DataNodeConfig {
    DataNodeServerConfig server;
    NameNodeEndpointConfig namenode;
    StorageConfig storage;
    HeartbeatConfig heartbeat;
    BlockReportConfig block_report;
};

struct ClientConfig {
    NameNodeEndpointConfig namenode;
    std::string user = "default";
    uint64_t default_block_size = 134217728;
    uint64_t io_buffer_size = 1048576;
    uint32_t rpc_timeout_ms = 5000;
    uint32_t max_retry = 3;
};

pl::Result<NameNodeConfig> load_namenode_config(const std::string& path);
pl::Result<DataNodeConfig> load_datanode_config(const std::string& path);
pl::Result<ClientConfig> load_client_config(const std::string& path);

} // namespace pl::minidfs
