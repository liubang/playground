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
// Created: 2026/05/10 23:30

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fmt/core.h>
#include <gflags/gflags.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/ascii_table/pretty.h"
#include "cpp/pl/minidfs/client/dfs_client.h"

DEFINE_string(namenode, "127.0.0.1:8020", "NameNode address (host:port)");
DEFINE_int32(rpc_timeout_ms, 5000, "RPC timeout in milliseconds");
DEFINE_int32(replication, 3, "Default replication factor");
DEFINE_int64(block_size, 128 * 1024 * 1024, "Default block size in bytes");

namespace {

using pl::minidfs::DfsClient;
using pl::minidfs::DfsClientConfig;
using pl::minidfs::FileStatus;
using namespace pl::pretty;

/// Generate a unique client_id.
std::string generate_client_id() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fmt::format("cli-{}-{}", ::getpid(), now);
}

/// Format timestamp (ms) as human-readable string.
std::string format_time(uint64_t mtime_ms) {
    if (mtime_ms == 0) {
        return "-";
    }
    auto secs = static_cast<time_t>(mtime_ms / 1000);
    std::tm tm{};
    ::localtime_r(&secs, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M");
    return oss.str();
}

/// Format file size in human-readable form.
std::string format_size(uint64_t size) {
    if (size == 0) {
        return "0";
    }
    constexpr uint64_t KB = 1024;
    constexpr uint64_t MB = KB * 1024;
    constexpr uint64_t GB = MB * 1024;
    if (size >= GB) {
        return fmt::format("{:.1f}G", static_cast<double>(size) / static_cast<double>(GB));
    }
    if (size >= MB) {
        return fmt::format("{:.1f}M", static_cast<double>(size) / static_cast<double>(MB));
    }
    if (size >= KB) {
        return fmt::format("{:.1f}K", static_cast<double>(size) / static_cast<double>(KB));
    }
    return fmt::format("{}B", size);
}

/// Format permission as octal string.
std::string format_permission(uint32_t perm) {
    return fmt::format("{:04o}", perm);
}

std::optional<uint64_t> parse_uint64(std::string_view value) {
    if (value.empty() || value.front() == '-') {
        return std::nullopt;
    }
    size_t parsed = 0;
    try {
        auto number = std::stoull(std::string(value), &parsed);
        if (parsed != value.size()) {
            return std::nullopt;
        }
        return number;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Create the DfsClient from flags.
std::unique_ptr<DfsClient> make_client() {
    DfsClientConfig config;
    config.namenode_address = FLAGS_namenode;
    config.rpc_timeout_ms = FLAGS_rpc_timeout_ms;
    config.max_retry = 3;
    config.client_id = generate_client_id();
    config.replication = static_cast<uint32_t>(FLAGS_replication);
    config.block_size = static_cast<uint64_t>(FLAGS_block_size);
    return DfsClient::create(std::move(config));
}

int cmd_mkdir(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs -mkdir <path>\n";
        return 1;
    }
    auto result = client->mkdir(argv[0]);
    if (result.hasError()) {
        std::cerr << "mkdir failed: " << result.error().describe() << "\n";
        return 1;
    }
    return 0;
}

/// Options for ls command, mirroring hdfs dfs -ls behavior.
struct LsOptions {
    bool dir_only = false;  // -d: show directory itself, not contents
    bool human = false;     // -h: human-readable sizes
    bool recursive = false; // -R: recursive listing
    bool sort_time = false; // -t: sort by modification time (newest first)
    bool sort_size = false; // -S: sort by size (largest first)
    bool reverse = false;   // -r: reverse sort order
};

/// Format size as raw bytes string.
std::string format_size_raw(uint64_t size) {
    return std::to_string(size);
}

/// Print a listing of entries in hdfs-style format.
void print_ls_entries(const std::vector<FileStatus>& entries, const LsOptions& opts) {
    if (entries.empty()) {
        return;
    }

    // Build a mutable copy for sorting
    std::vector<FileStatus> sorted = entries;

    if (opts.sort_time) {
        std::sort(sorted.begin(), sorted.end(), [](const FileStatus& a, const FileStatus& b) {
            return a.mtime_ms > b.mtime_ms; // newest first
        });
    } else if (opts.sort_size) {
        std::sort(sorted.begin(), sorted.end(), [](const FileStatus& a, const FileStatus& b) {
            return a.length > b.length; // largest first
        });
    }

    if (opts.reverse) {
        std::reverse(sorted.begin(), sorted.end());
    }

    pl::pretty::Table t;
    t.header({"Type", "Perm", "Repl", "Owner", "Group", "Size", "Modified", "Name"});
    for (const auto& entry : sorted) {
        std::string size_str;
        if (entry.is_dir) {
            size_str = "0";
        } else if (opts.human) {
            size_str = format_size(entry.length);
        } else {
            size_str = format_size_raw(entry.length);
        }
        t.data({
            entry.is_dir ? "d" : "-",
            format_permission(entry.permission),
            std::to_string(entry.replication),
            entry.owner,
            entry.group,
            size_str,
            format_time(entry.mtime_ms),
            entry.path,
        });
    }
    t.render();
}

/// Recursively list directory contents.
int ls_recursive(DfsClient* client, const std::string& path, const LsOptions& opts) {
    auto result = client->ls(path);
    if (result.hasError()) {
        std::cerr << "ls: cannot access '" << path << "': " << result.error().describe() << "\n";
        return 1;
    }

    const auto& entries = result.value();
    // Print header for this directory (like hdfs does for -R)
    std::cout << path << ":\n";
    print_ls_entries(entries, opts);
    std::cout << "\n";

    // Recurse into subdirectories
    for (const auto& entry : entries) {
        if (entry.is_dir) {
            ls_recursive(client, entry.path, opts);
        }
    }
    return 0;
}

int cmd_ls(DfsClient* client, int argc, char* argv[]) {
    LsOptions opts;
    std::string path = "/";

    // Parse options
    int i = 0;
    for (; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-d") {
            opts.dir_only = true;
        } else if (arg == "-h") {
            opts.human = true;
        } else if (arg == "-R") {
            opts.recursive = true;
        } else if (arg == "-t") {
            opts.sort_time = true;
        } else if (arg == "-S") {
            opts.sort_size = true;
        } else if (arg == "-r") {
            opts.reverse = true;
        } else if (arg[0] == '-' && arg.size() > 1) {
            // Handle combined flags like -hRt
            for (size_t j = 1; j < arg.size(); ++j) {
                switch (arg[j]) {
                    case 'd':
                        opts.dir_only = true;
                        break;
                    case 'h':
                        opts.human = true;
                        break;
                    case 'R':
                        opts.recursive = true;
                        break;
                    case 't':
                        opts.sort_time = true;
                        break;
                    case 'S':
                        opts.sort_size = true;
                        break;
                    case 'r':
                        opts.reverse = true;
                        break;
                    default:
                        std::cerr << "ls: unknown option: -" << arg[j] << "\n";
                        return 1;
                }
            }
        } else {
            // First non-option argument is the path
            path = arg;
            break;
        }
    }

    // -d: show the directory entry itself (via stat), not its contents
    if (opts.dir_only) {
        auto result = client->stat(path);
        if (result.hasError()) {
            std::cerr << "ls: cannot access '" << path << "': " << result.error().describe()
                      << "\n";
            return 1;
        }
        std::vector<FileStatus> entries = {result.value()};
        print_ls_entries(entries, opts);
        return 0;
    }

    // -R: recursive listing
    if (opts.recursive) {
        return ls_recursive(client, path, opts);
    }

    // Normal listing
    auto result = client->ls(path);
    if (result.hasError()) {
        std::cerr << "ls: cannot access '" << path << "': " << result.error().describe() << "\n";
        return 1;
    }

    const auto& entries = result.value();
    if (entries.empty()) {
        return 0;
    }

    print_ls_entries(entries, opts);
    return 0;
}

int cmd_stat(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs -stat <path>\n";
        return 1;
    }
    auto result = client->stat(argv[0]);
    if (result.hasError()) {
        std::cerr << "stat failed: " << result.error().describe() << "\n";
        return 1;
    }

    const auto& fs = result.value();
    // clang-format off
    auto t = header("Property", "Value")
           | row("Path",       fs.path)
           | row("Type",       fs.is_dir ? "directory" : "file")
           | row("Inode ID",   std::to_string(fs.inode_id))
           | row("Size",       format_size(fs.length))
           | row("Replication", std::to_string(fs.replication))
           | row("Block Size", format_size(fs.block_size))
           | row("Owner",      fs.owner)
           | row("Group",      fs.group)
           | row("Permission", format_permission(fs.permission))
           | row("Modified",   format_time(fs.mtime_ms));
    // clang-format on
    t.render();
    return 0;
}

int cmd_rm(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs -rm [-r] <path>\n";
        return 1;
    }

    bool recursive = false;
    int path_idx = 0;
    if (std::string(argv[0]) == "-r" || std::string(argv[0]) == "-R") {
        recursive = true;
        path_idx = 1;
    }
    if (path_idx >= argc) {
        std::cerr << "Usage: minidfs -rm [-r] <path>\n";
        return 1;
    }

    auto result = client->rm(argv[path_idx], recursive);
    if (result.hasError()) {
        std::cerr << "rm failed: " << result.error().describe() << "\n";
        return 1;
    }
    return 0;
}

int cmd_mv(DfsClient* client, int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: minidfs -mv <src> <dst>\n";
        return 1;
    }
    auto result = client->mv(argv[0], argv[1]);
    if (result.hasError()) {
        std::cerr << "mv failed: " << result.error().describe() << "\n";
        return 1;
    }
    return 0;
}

int cmd_put(DfsClient* client, int argc, char* argv[]) {
    bool overwrite = false;
    int path_idx = 0;
    if (argc > 0 && std::string(argv[0]) == "-f") {
        overwrite = true;
        path_idx = 1;
    }
    if (argc - path_idx < 2) {
        std::cerr << "Usage: minidfs -put [-f] <local_path> <dfs_path>\n";
        return 1;
    }
    auto result = client->put(argv[path_idx], argv[path_idx + 1], overwrite);
    if (result.hasError()) {
        std::cerr << "put failed: " << result.error().describe() << "\n";
        return 1;
    }
    std::cout << "Uploaded " << argv[path_idx] << " -> " << argv[path_idx + 1] << "\n";
    return 0;
}

int cmd_append(DfsClient* client, int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: minidfs append <local_path> <dfs_path>\n";
        return 1;
    }
    auto result = client->append(argv[0], argv[1]);
    if (result.hasError()) {
        std::cerr << "append failed: " << result.error().describe() << "\n";
        return 1;
    }
    std::cout << "Appended " << argv[0] << " -> " << argv[1] << "\n";
    return 0;
}

int cmd_truncate(DfsClient* client, int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: minidfs truncate <length> <dfs_path>\n";
        return 1;
    }
    auto length = parse_uint64(argv[0]);
    if (!length.has_value()) {
        std::cerr << "truncate failed: invalid length\n";
        return 1;
    }
    auto result = client->truncate(argv[1], *length);
    if (result.hasError()) {
        std::cerr << "truncate failed: " << result.error().describe() << "\n";
        return 1;
    }
    return 0;
}

int cmd_setrep(DfsClient* client, int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: minidfs setrep <replication> <dfs_path>\n";
        return 1;
    }
    auto replication = parse_uint64(argv[0]);
    if (!replication.has_value() || *replication > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "setrep failed: invalid replication factor\n";
        return 1;
    }
    auto result = client->setrep(argv[1], static_cast<uint32_t>(*replication));
    if (result.hasError()) {
        std::cerr << "setrep failed: " << result.error().describe() << "\n";
        return 1;
    }
    return 0;
}

int cmd_get(DfsClient* client, int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: minidfs -get <dfs_path> <local_path>\n";
        return 1;
    }
    auto result = client->get(argv[0], argv[1]);
    if (result.hasError()) {
        std::cerr << "get failed: " << result.error().describe() << "\n";
        return 1;
    }
    std::cout << "Downloaded " << argv[0] << " -> " << argv[1] << "\n";
    return 0;
}

// Admin commands

int cmd_fsinfo(DfsClient* client, int /*argc*/, char* /*argv*/[]) {
    auto result = client->get_cluster_info();
    if (result.hasError()) {
        std::cerr << "fsinfo failed: " << result.error().describe() << "\n";
        return 1;
    }

    const auto& info = result.value();
    // clang-format off
    auto t = header("Property",              "Value")
           | row("Total Capacity",           format_size(info.total_capacity_bytes))
           | row("Used",                     format_size(info.used_bytes))
           | row("Free",                     format_size(info.free_bytes))
           | row("Live DataNodes",           std::to_string(info.live_datanodes))
           | row("Dead DataNodes",           std::to_string(info.dead_datanodes))
           | row("Total Blocks",             std::to_string(info.total_blocks))
           | row("Total Files",              std::to_string(info.total_files))
           | row("Total Directories",        std::to_string(info.total_directories))
           | row("Under-Replicated Blocks",  std::to_string(info.under_replicated_blocks));
    // clang-format on
    t.render();
    return 0;
}

int cmd_datanodes(DfsClient* client, int argc, char* argv[]) {
    bool include_dead = false;
    if (argc > 0 && (std::string(argv[0]) == "--all" || std::string(argv[0]) == "-a")) {
        include_dead = true;
    }

    auto result = client->list_datanodes(include_dead);
    if (result.hasError()) {
        std::cerr << "datanodes failed: " << result.error().describe() << "\n";
        return 1;
    }

    const auto& dns = result.value();
    if (dns.empty()) {
        std::cout << "(no datanodes registered)\n";
        return 0;
    }

    pl::pretty::Table t;
    t.header({"ID",
              "Hostname",
              "IP",
              "RPC Port",
              "Data Port",
              "Rack",
              "State",
              "Capacity",
              "Used",
              "Free",
              "Blocks",
              "Last HB"});
    for (const auto& dn : dns) {
        t.data({
            std::to_string(dn.datanode_id),
            dn.hostname,
            dn.ip,
            std::to_string(dn.rpc_port),
            std::to_string(dn.data_port),
            dn.rack,
            dn.state,
            format_size(dn.capacity_bytes),
            format_size(dn.used_bytes),
            format_size(dn.free_bytes),
            std::to_string(dn.block_count),
            format_time(dn.last_heartbeat_ms),
        });
    }
    t.render();
    return 0;
}

int cmd_datanode(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs datanode <datanode_id>\n";
        return 1;
    }

    uint64_t datanode_id = std::stoull(argv[0]);
    auto result = client->get_datanode_info(datanode_id);
    if (result.hasError()) {
        std::cerr << "datanode failed: " << result.error().describe() << "\n";
        return 1;
    }

    const auto& dn = result.value();
    // clang-format off
    auto t = header("Property",       "Value")
           | row("DataNode ID",       std::to_string(dn.datanode_id))
           | row("UUID",              dn.uuid)
           | row("Hostname",          dn.hostname)
           | row("IP",                dn.ip)
           | row("RPC Port",          std::to_string(dn.rpc_port))
           | row("Data Port",         std::to_string(dn.data_port))
           | row("Rack",              dn.rack)
           | row("State",             dn.state)
           | row("Capacity",          format_size(dn.capacity_bytes))
           | row("Used",              format_size(dn.used_bytes))
           | row("Free",              format_size(dn.free_bytes))
           | row("Block Count",       std::to_string(dn.block_count))
           | row("Last Heartbeat",    format_time(dn.last_heartbeat_ms));
    // clang-format on
    t.render();
    return 0;
}

int cmd_inode(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs inode <inode_id | path>\n";
        return 1;
    }

    std::string arg = argv[0];
    pl::Result<DfsClient::InodeDetail> result = (arg[0] == '/')
                                                    ? client->get_inode_info_by_path(arg)
                                                    : client->get_inode_info(std::stoull(arg));

    if (result.hasError()) {
        std::cerr << "inode failed: " << result.error().describe() << "\n";
        return 1;
    }

    const auto& inode = result.value();
    // clang-format off
    auto t = header("Property",   "Value")
           | row("Inode ID",     std::to_string(inode.inode_id))
           | row("Type",         inode.type)
           | row("Parent ID",    std::to_string(inode.parent_id))
           | row("Name",         inode.name)
           | row("Owner",        inode.owner)
           | row("Group",        inode.group)
           | row("Permission",   format_permission(inode.permission))
           | row("Length",       format_size(inode.length))
           | row("Replication",  std::to_string(inode.replication))
           | row("Block Size",   format_size(inode.block_size))
           | row("State",        inode.state)
           | row("Created",      format_time(inode.ctime_ms))
           | row("Modified",     format_time(inode.mtime_ms));
    if (inode.type == "file") {
        t = t | row("Block Count", std::to_string(inode.block_count));
    } else {
        t = t | row("Child Count", std::to_string(inode.child_count));
    }
    // clang-format on
    t.render();
    return 0;
}

int cmd_blocks(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs blocks <inode_id | path>\n";
        return 1;
    }

    std::string arg = argv[0];
    auto result = (arg[0] == '/') ? client->get_file_blocks_by_path(arg)
                                  : client->get_file_blocks(std::stoull(arg));

    if (result.hasError()) {
        std::cerr << "blocks failed: " << result.error().describe() << "\n";
        return 1;
    }

    const auto& blocks = result.value();
    if (blocks.empty()) {
        std::cout << "(no blocks)\n";
        return 0;
    }

    pl::pretty::Table t;
    t.header(
        {"Block ID", "Index", "GenStamp", "Length", "State", "Desired", "Actual", "Locations"});
    for (const auto& b : blocks) {
        std::string locs;
        for (size_t i = 0; i < b.locations.size(); ++i) {
            if (i > 0)
                locs += ", ";
            locs += b.locations[i].host + ":" + std::to_string(b.locations[i].data_port);
        }
        t.data({
            std::to_string(b.block_id),
            std::to_string(b.block_index),
            std::to_string(b.generation_stamp),
            format_size(b.length),
            b.state,
            std::to_string(b.desired_replicas),
            std::to_string(b.actual_replicas),
            locs,
        });
    }
    t.render();
    return 0;
}

int cmd_block(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs block <block_id>\n";
        return 1;
    }

    uint64_t block_id = std::stoull(argv[0]);
    auto result = client->get_block_info(block_id);
    if (result.hasError()) {
        std::cerr << "block failed: " << result.error().describe() << "\n";
        return 1;
    }

    const auto& block = result.value();
    // clang-format off
    auto t = header("Property",          "Value")
           | row("Block ID",            std::to_string(block.block_id))
           | row("Inode ID",            std::to_string(block.inode_id))
           | row("Block Index",         std::to_string(block.block_index))
           | row("Generation Stamp",    std::to_string(block.generation_stamp))
           | row("Length",              format_size(block.length))
           | row("State",               block.state)
           | row("Desired Replicas",    std::to_string(block.desired_replicas));
    // clang-format on
    t.render();

    if (!block.replicas.empty()) {
        std::cout << "\nReplicas:\n";
        pl::pretty::Table rep_table;
        rep_table.header({"DataNode ID", "Hostname", "State", "Length", "GenStamp", "Report Time"});
        for (const auto& r : block.replicas) {
            rep_table.data({
                std::to_string(r.datanode_id),
                r.hostname,
                r.state,
                format_size(r.length),
                std::to_string(r.generation_stamp),
                format_time(r.report_time_ms),
            });
        }
        rep_table.render();
    }
    return 0;
}

void print_usage() {
    std::cerr << "Usage: minidfs [options] <command> [args...]\n"
              << "\n"
              << "File System Commands:\n"
              << "  mkdir <path>               Create a directory\n"
              << "  ls [-d] [-h] [-R] [-t] [-S] [-r] [path]\n"
              << "                             List directory contents\n"
              << "      -d  Show directory itself, not contents\n"
              << "      -h  Human-readable file sizes\n"
              << "      -R  Recursively list subdirectories\n"
              << "      -t  Sort by modification time (newest first)\n"
              << "      -S  Sort by file size (largest first)\n"
              << "      -r  Reverse sort order\n"
              << "  stat <path>                Show file/directory status\n"
              << "  rm [-r] <path>             Delete a file or directory\n"
              << "  mv <src> <dst>             Rename/move a file or directory\n"
              << "  put [-f] <local> <dfs_path> Upload a local file to DFS\n"
              << "  append <local> <dfs_path>  Append a local file to DFS\n"
              << "  truncate <len> <dfs_path>  Shrink a DFS file\n"
              << "  setrep <n> <dfs_path>      Set a DFS file's replication factor\n"
              << "  get <dfs_path> <local>     Download a DFS file to local\n"
              << "\n"
              << "Admin Commands:\n"
              << "  fsinfo                     Show cluster summary\n"
              << "  datanodes [-a|--all]       List all datanodes\n"
              << "  datanode <id>              Show datanode details\n"
              << "  inode <id|path>            Show inode details\n"
              << "  blocks <id|path>           List blocks of a file\n"
              << "  block <block_id>           Show block details and replicas\n"
              << "\n"
              << "Options:\n"
              << "  -namenode=<host:port>      NameNode address (default: 127.0.0.1:8020)\n"
              << "  -rpc_timeout_ms=<ms>       RPC timeout (default: 5000)\n"
              << "  -replication=<n>           Replication factor (default: 3)\n"
              << "  -block_size=<bytes>        Block size (default: 128MB)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    gflags::SetUsageMessage("MiniDFS command-line client");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (argc < 2) {
        print_usage();
        return 1;
    }

    auto client = make_client();
    if (!client) {
        std::cerr << "Failed to connect to NameNode at " << FLAGS_namenode << "\n";
        return 1;
    }

    std::string cmd = argv[1];

    // Remaining args after the command
    int cmd_argc = argc - 2;
    char** cmd_argv = argv + 2;

    if (cmd == "-mkdir" || cmd == "mkdir") {
        return cmd_mkdir(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-ls" || cmd == "ls") {
        return cmd_ls(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-stat" || cmd == "stat") {
        return cmd_stat(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-rm" || cmd == "rm") {
        return cmd_rm(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-mv" || cmd == "mv") {
        return cmd_mv(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-put" || cmd == "put") {
        return cmd_put(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "append") {
        return cmd_append(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-truncate" || cmd == "truncate") {
        return cmd_truncate(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-setrep" || cmd == "setrep") {
        return cmd_setrep(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-get" || cmd == "get") {
        return cmd_get(client.get(), cmd_argc, cmd_argv);
    }

    // Admin commands
    if (cmd == "fsinfo") {
        return cmd_fsinfo(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "datanodes") {
        return cmd_datanodes(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "datanode") {
        return cmd_datanode(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "inode") {
        return cmd_inode(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "blocks") {
        return cmd_blocks(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "block") {
        return cmd_block(client.get(), cmd_argc, cmd_argv);
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    print_usage();
    return 1;
}
