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

#include <brpc/server.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>

#include "cpp/pl/minidfs/metadata/mysql_connection_pool.h"
#include "cpp/pl/minidfs/metadata/mysql_metadata_store.h"
#include "cpp/pl/minidfs/namenode/admin_service_impl.h"
#include "cpp/pl/minidfs/namenode/block_manager.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/minidfs/namenode/lease_manager.h"
#include "cpp/pl/minidfs/namenode/namenode_service_impl.h"
#include "cpp/pl/minidfs/namenode/namenode_maintenance.h"
#include "cpp/pl/minidfs/namenode/namespace_manager.h"
#include "cpp/pl/minidfs/namenode/placement_manager.h"
#include "cpp/pl/minidfs/namenode/replication_manager.h"

DEFINE_int32(port, 9000, "NameNode RPC service port");
DEFINE_string(mysql_host, "127.0.0.1", "MySQL host");
DEFINE_int32(mysql_port, 3306, "MySQL port");
DEFINE_string(mysql_user, "root", "MySQL user");
DEFINE_string(mysql_password, "", "MySQL password");
DEFINE_string(mysql_database, "minidfs", "MySQL database name");
DEFINE_int32(mysql_pool_size, 8, "MySQL connection pool size");
DEFINE_int64(lease_recovery_interval_ms, 1000, "Lease recovery scan interval");
DEFINE_int64(datanode_scan_interval_ms, 3000, "DataNode liveness scan interval");
DEFINE_int64(replication_scan_interval_ms, 30000, "Replica repair scan interval");

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_lease_recovery_interval_ms <= 0 || FLAGS_datanode_scan_interval_ms <= 0 ||
        FLAGS_replication_scan_interval_ms <= 0) {
        XLOG(FATAL, "NameNode maintenance intervals must be positive");
        return 1;
    }

    // Initialize MySQL connection pool
    pl::minidfs::MySQLConfig mysql_config{
        .host = FLAGS_mysql_host,
        .port = static_cast<uint16_t>(FLAGS_mysql_port),
        .user = FLAGS_mysql_user,
        .password = FLAGS_mysql_password,
        .database = FLAGS_mysql_database,
        .pool_size = static_cast<uint32_t>(FLAGS_mysql_pool_size),
    };

    auto pool_result = pl::minidfs::MySQLConnectionPool::create(mysql_config);
    if (pool_result.hasError()) {
        XLOGF(FATAL, "failed to initialize MySQL pool: {}", pool_result.error().describe());
        return 1;
    }
    auto pool = std::move(pool_result.value());

    // Create metadata store
    auto store_result = pl::minidfs::MySQLMetadataStore::create(pool);
    if (store_result.hasError()) {
        XLOGF(FATAL, "failed to create metadata store: {}", store_result.error().describe());
        return 1;
    }
    auto& metadata_store = *store_result.value();

    // Create managers
    pl::minidfs::NamespaceManager ns_mgr(&metadata_store);
    pl::minidfs::DataNodeManager dn_mgr(&metadata_store);
    pl::minidfs::PlacementManager placement_mgr(&dn_mgr);
    pl::minidfs::BlockManager block_mgr(&metadata_store, &placement_mgr);
    pl::minidfs::LeaseManager lease_mgr(&metadata_store);
    pl::minidfs::ReplicationManager replication_mgr(&metadata_store, &placement_mgr);
    pl::minidfs::NameNodeMaintenance maintenance(
        {
            .lease_recovery_interval_ms = static_cast<uint64_t>(FLAGS_lease_recovery_interval_ms),
            .datanode_scan_interval_ms = static_cast<uint64_t>(FLAGS_datanode_scan_interval_ms),
            .replication_scan_interval_ms = static_cast<uint64_t>(FLAGS_replication_scan_interval_ms),
        },
        &metadata_store,
        &ns_mgr,
        &block_mgr,
        &dn_mgr,
        &replication_mgr);

    // Create service implementations
    pl::minidfs::NameNodeServiceImpl namenode_service(
        &ns_mgr, &block_mgr, &lease_mgr, &metadata_store);
    pl::minidfs::DataNodeProtocolServiceImpl datanode_protocol_service(
        &dn_mgr, &block_mgr, &maintenance);
    pl::minidfs::AdminServiceImpl admin_service(&ns_mgr, &dn_mgr, &metadata_store);

    // Start brpc server
    brpc::Server server;

    if (server.AddService(&namenode_service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        XLOG(FATAL, "failed to add NameNodeService");
        return 1;
    }
    if (server.AddService(&datanode_protocol_service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        XLOG(FATAL, "failed to add DataNodeProtocolService");
        return 1;
    }
    if (server.AddService(&admin_service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        XLOG(FATAL, "failed to add AdminService");
        return 1;
    }

    brpc::ServerOptions options;
    if (server.Start(FLAGS_port, &options) != 0) {
        XLOGF(FATAL, "failed to start server on port {}", FLAGS_port);
        return 1;
    }

    XLOGF(INFO, "MiniDFS NameNode started on port {}", FLAGS_port);
    maintenance.start();
    server.RunUntilAskedToQuit();
    maintenance.stop();

    return 0;
}
