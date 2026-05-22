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
// Created: 2026/05/11 12:00

// format 命令：初始化 MiniDFS 的 MySQL 元数据库。
// 读取 schema.sql 文件，逐条执行 DDL 语句，完成建库、建表和初始数据插入。

#include <fstream>
#include <gflags/gflags.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cpp/pl/minidfs/metadata/mysql_connection_pool.h"

DEFINE_string(mysql_host, "127.0.0.1", "MySQL host");
DEFINE_int32(mysql_port, 3306, "MySQL port");
DEFINE_string(mysql_user, "root", "MySQL user");
DEFINE_string(mysql_password, "", "MySQL password");
DEFINE_string(mysql_database, "minidfs", "MySQL database name");
DEFINE_string(schema_file, "", "Path to schema.sql file");
DEFINE_bool(force, false, "Force format even if database already exists (drops and recreates)");

namespace {

/// 读取文件全部内容
std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

/// 将 SQL 文件按分号拆分为独立的语句，忽略注释行和空语句
std::vector<std::string> split_statements(const std::string& sql) {
    std::vector<std::string> statements;
    std::string current;

    std::istringstream stream(sql);
    std::string line;

    while (std::getline(stream, line)) {
        // 跳过纯注释行
        auto trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
            trimmed.erase(trimmed.begin());
        }
        if (trimmed.starts_with("--") || trimmed.empty()) {
            continue;
        }

        current += line;
        current += '\n';

        // 如果行末含分号，表示语句结束
        if (line.find(';') != std::string::npos) {
            // 去除尾部空白
            while (!current.empty() &&
                   (current.back() == '\n' || current.back() == ' ' || current.back() == '\t')) {
                current.pop_back();
            }
            if (!current.empty()) {
                statements.push_back(std::move(current));
            }
            current.clear();
        }
    }

    // 处理最后一条没有分号结尾的语句（如果有）
    while (!current.empty() &&
           (current.back() == '\n' || current.back() == ' ' || current.back() == '\t')) {
        current.pop_back();
    }
    if (!current.empty()) {
        statements.push_back(std::move(current));
    }

    return statements;
}

} // namespace

int main(int argc, char* argv[]) {
    gflags::SetUsageMessage(
        "MiniDFS format - Initialize the metadata database.\n"
        "Usage: format --schema_file=<path> [--mysql_host=...] [--mysql_port=...]\n"
        "              [--mysql_user=...] [--mysql_password=...] [--force]");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_schema_file.empty()) {
        std::cerr << "Error: --schema_file is required.\n";
        std::cerr << "  Example: format --schema_file=cpp/pl/minidfs/metadata/schema.sql\n";
        return 1;
    }

    // 读取 schema.sql
    std::string schema_sql = read_file(FLAGS_schema_file);
    if (schema_sql.empty()) {
        std::cerr << "Error: failed to read schema file: " << FLAGS_schema_file << "\n";
        return 1;
    }

    std::cout << "MiniDFS format: initializing metadata database...\n";

    // 创建 MySQL 连接（不指定 database，以便执行 CREATE DATABASE）
    pl::minidfs::MySQLConfig config{
        .host = FLAGS_mysql_host,
        .port = static_cast<uint16_t>(FLAGS_mysql_port),
        .user = FLAGS_mysql_user,
        .password = FLAGS_mysql_password,
        .database = "", // 初始连接不指定 database，由 schema.sql 内的 USE 语句切换
        .pool_size = 1,
    };

    auto pool_result = pl::minidfs::MySQLConnectionPool::create(config);
    if (pool_result.hasError()) {
        std::cerr << "Error: failed to connect to MySQL: " << pool_result.error().describe()
                  << "\n";
        std::cerr << "  Host: " << FLAGS_mysql_host << ":" << FLAGS_mysql_port << "\n";
        std::cerr << "  User: " << FLAGS_mysql_user << "\n";
        return 1;
    }
    auto pool = std::move(pool_result.value());

    // 获取连接
    auto conn_result = pool->acquire();
    if (conn_result.hasError()) {
        std::cerr << "Error: failed to acquire connection: " << conn_result.error().describe()
                  << "\n";
        return 1;
    }
    auto conn = std::move(conn_result.value());

    // 如果 --force，先删除并重建数据库
    if (FLAGS_force) {
        std::cout << "  [force] Dropping database '" << FLAGS_mysql_database << "' if exists...\n";
        auto drop_result = conn.execute("DROP DATABASE IF EXISTS " + FLAGS_mysql_database);
        if (drop_result.hasError()) {
            std::cerr << "Warning: failed to drop database: " << drop_result.error().describe()
                      << "\n";
        }
    }

    // 逐条执行 SQL 语句
    auto statements = split_statements(schema_sql);
    int executed = 0;
    int failed = 0;

    for (const auto& stmt : statements) {
        auto result = conn.execute(stmt);
        if (result.hasError()) {
            std::cerr << "  [FAIL] " << result.error().describe() << "\n";
            std::cerr << "         Statement: " << stmt.substr(0, 80) << "...\n";
            ++failed;
        } else {
            ++executed;
        }
    }

    std::cout << "\n";
    std::cout << "Format complete:\n";
    std::cout << "  Statements executed: " << executed << "\n";
    if (failed > 0) {
        std::cout << "  Statements failed:   " << failed << "\n";
        std::cerr << "\nWarning: " << failed
                  << " statement(s) failed. Database may be partially initialized.\n";
        return 1;
    }

    std::cout << "  Database '" << FLAGS_mysql_database << "' is ready.\n";
    return 0;
}
