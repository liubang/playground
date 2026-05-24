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

#include "cpp/pl/minidfs/common/config.h"

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/pl/minidfs/common/error_code.h"

namespace pl::minidfs {

namespace {

std::string trim(std::string_view sv) {
    auto start = sv.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos)
        return {};
    auto end = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(start, end - start + 1));
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Simple YAML parser: supports two-level nested key: value and list items
struct YamlMap {
    // "section.key" -> "value" flat mapping
    std::unordered_map<std::string, std::string> values;
    // "section" -> [values] list items
    std::unordered_map<std::string, std::vector<std::string>> lists;

    std::string get(const std::string& key, const std::string& def = {}) const {
        auto it = values.find(key);
        return it != values.end() ? it->second : def;
    }

    uint64_t get_u64(const std::string& key, uint64_t def = 0) const {
        auto it = values.find(key);
        if (it == values.end())
            return def;
        return std::stoull(it->second);
    }

    uint32_t get_u32(const std::string& key, uint32_t def = 0) const {
        return static_cast<uint32_t>(get_u64(key, def));
    }

    uint16_t get_u16(const std::string& key, uint16_t def = 0) const {
        return static_cast<uint16_t>(get_u64(key, def));
    }

    std::vector<std::string> get_list(const std::string& key) const {
        auto it = lists.find(key);
        return it != lists.end() ? it->second : std::vector<std::string>{};
    }
};

pl::Result<YamlMap> parse_yaml(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kIOError),
                             "cannot open config file: " + path);
    }

    YamlMap yaml;
    std::string line;
    std::string section;

    while (std::getline(file, line)) {
        // Skip comments and blank lines
        auto trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        // Calculate indentation
        auto indent = line.find_first_not_of(' ');

        if (indent == 0) {
            // Top-level key
            auto colon = trimmed.find(':');
            if (colon == std::string::npos)
                continue;
            auto key = trim(trimmed.substr(0, colon));
            auto val = trim(trimmed.substr(colon + 1));
            if (val.empty()) {
                // Section header
                section = key;
            } else {
                yaml.values[key] = unquote(val);
            }
        } else {
            // Indented content: list items or nested keys.
            if (trimmed.starts_with("- ")) {
                // List item, append to current section
                auto val = unquote(trim(trimmed.substr(2)));
                yaml.lists[section].push_back(val);
            } else {
                auto colon = trimmed.find(':');
                if (colon == std::string::npos)
                    continue;
                auto key = trim(trimmed.substr(0, colon));
                auto val = trim(trimmed.substr(colon + 1));
                if (val.empty()) {
                    // Sub-section (3-level nesting: join with parent prefix)
                    section = section.substr(0, section.find('.')) + "." + key;
                } else {
                    auto full_key = section + "." + key;
                    yaml.values[full_key] = unquote(val);
                }
            }
        }
    }
    return yaml;
}

} // namespace

pl::Result<NameNodeConfig> load_namenode_config(const std::string& path) {
    auto yaml_result = parse_yaml(path);
    if (yaml_result.hasError()) {
        return folly::makeUnexpected(yaml_result.error());
    }
    auto& yaml = yaml_result.value();

    NameNodeConfig cfg;
    cfg.server.host = yaml.get("server.host", cfg.server.host);
    cfg.server.port = yaml.get_u32("server.port", cfg.server.port);
    cfg.mysql.host = yaml.get("mysql.host", cfg.mysql.host);
    cfg.mysql.port = yaml.get_u16("mysql.port", cfg.mysql.port);
    cfg.mysql.user = yaml.get("mysql.user", cfg.mysql.user);
    cfg.mysql.password = yaml.get("mysql.password", cfg.mysql.password);
    cfg.mysql.database = yaml.get("mysql.database", cfg.mysql.database);
    cfg.mysql.pool_size = yaml.get_u32("mysql.pool_size", cfg.mysql.pool_size);
    cfg.filesystem.default_replication =
        yaml.get_u32("filesystem.default_replication", cfg.filesystem.default_replication);
    cfg.filesystem.default_block_size =
        yaml.get_u64("filesystem.default_block_size", cfg.filesystem.default_block_size);
    cfg.filesystem.default_chunk_size =
        yaml.get_u64("filesystem.default_chunk_size", cfg.filesystem.default_chunk_size);
    cfg.filesystem.min_write_replica =
        yaml.get_u32("filesystem.min_write_replica", cfg.filesystem.min_write_replica);
    cfg.heartbeat.stale_timeout_ms =
        yaml.get_u64("heartbeat.stale_timeout_ms", cfg.heartbeat.stale_timeout_ms);
    cfg.heartbeat.dead_timeout_ms =
        yaml.get_u64("heartbeat.dead_timeout_ms", cfg.heartbeat.dead_timeout_ms);
    cfg.heartbeat.interval_ms = yaml.get_u64("heartbeat.interval_ms", cfg.heartbeat.interval_ms);
    cfg.replication.scan_interval_ms =
        yaml.get_u64("replication.scan_interval_ms", cfg.replication.scan_interval_ms);
    cfg.replication.max_replication_tasks_per_round =
        yaml.get_u32("replication.max_replication_tasks_per_round",
                     cfg.replication.max_replication_tasks_per_round);
    return cfg;
}

pl::Result<DataNodeConfig> load_datanode_config(const std::string& path) {
    auto yaml_result = parse_yaml(path);
    if (yaml_result.hasError()) {
        return folly::makeUnexpected(yaml_result.error());
    }
    auto& yaml = yaml_result.value();

    DataNodeConfig cfg;
    cfg.server.host = yaml.get("server.host", cfg.server.host);
    cfg.server.data_port = yaml.get_u32("server.data_port", cfg.server.data_port);
    cfg.server.rpc_port = yaml.get_u32("server.rpc_port", cfg.server.rpc_port);
    cfg.namenode.host = yaml.get("namenode.host", cfg.namenode.host);
    cfg.namenode.port = yaml.get_u32("namenode.port", cfg.namenode.port);
    cfg.storage.data_dirs = yaml.get_list("storage");
    if (cfg.storage.data_dirs.empty()) {
        auto single = yaml.get("storage.data_dir", "");
        if (!single.empty())
            cfg.storage.data_dirs.push_back(single);
    }
    cfg.storage.reserved_bytes = yaml.get_u64("storage.reserved_bytes", cfg.storage.reserved_bytes);
    cfg.heartbeat.interval_ms = yaml.get_u64("heartbeat.interval_ms", cfg.heartbeat.interval_ms);
    cfg.block_report.interval_ms =
        yaml.get_u64("block_report.interval_ms", cfg.block_report.interval_ms);
    return cfg;
}

pl::Result<ClientConfig> load_client_config(const std::string& path) {
    auto yaml_result = parse_yaml(path);
    if (yaml_result.hasError()) {
        return folly::makeUnexpected(yaml_result.error());
    }
    auto& yaml = yaml_result.value();

    ClientConfig cfg;
    cfg.namenode.host = yaml.get("namenode.host", cfg.namenode.host);
    cfg.namenode.port = yaml.get_u32("namenode.port", cfg.namenode.port);
    cfg.user = yaml.get("client.user", cfg.user);
    cfg.default_block_size = yaml.get_u64("client.default_block_size", cfg.default_block_size);
    cfg.io_buffer_size = yaml.get_u64("client.io_buffer_size", cfg.io_buffer_size);
    cfg.rpc_timeout_ms = yaml.get_u32("client.rpc_timeout_ms", cfg.rpc_timeout_ms);
    cfg.max_retry = yaml.get_u32("client.max_retry", cfg.max_retry);
    return cfg;
}

} // namespace pl::minidfs
