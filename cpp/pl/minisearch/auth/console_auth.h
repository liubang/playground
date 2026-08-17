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
// Created: 2026/08/17

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "cpp/pl/minisearch/auth/keystore.h"

namespace pl::minisearch::auth {

// Console 账号密码登录（与 API key 并行的第二条认证链路）。
//
// 用户存储：<data_dir>/auth/console_users.json（tmp+rename 原子写）：
//   {"users": [{"user", "salt", "hash"}, ...]}
// hash = sha256(salt + ":" + password)，明文密码不落盘。
//
// 首次启动（store 为空且配置了 bootstrap user/password）创建超级管理员；
// 之后以文件为准——改配置里的 bootstrap 密码不会覆盖已持久化的口令，
// 修改密码走 ChangePassword。
//
// Session token（"mss_" 前缀 + base62 随机串）仅存在于内存，带 TTL；
// 认证成功映射为 default 租户的 admin Principal，与 bootstrap key 等价。
class ConsoleAuth {
public:
    struct Options {
        std::string path;               // users 持久化文件；空 = 纯内存（测试）
        std::string bootstrap_user;     // 空 = 关闭账号密码登录
        std::string bootstrap_password; // 仅 store 为空时生效
        int64_t token_ttl_seconds = 86400;
    };

    explicit ConsoleAuth(Options opts);

    // 账号密码登录是否启用（配置了 bootstrap user 或 store 非空）。
    bool enabled() const { return !users_.empty(); }

    // 验证用户名密码，成功返回新 session token。
    std::optional<std::string> Login(const std::string& user, const std::string& password);

    // session token -> Principal；未知/过期返回 nullopt。
    std::optional<Principal> Authenticate(const std::string& token);

    void Logout(const std::string& token);

    // 凭旧密码修改；用户不存在或旧密码错误返回 false。
    bool ChangePassword(const std::string& user,
                        const std::string& old_password,
                        const std::string& new_password);

    int64_t token_ttl_seconds() const { return opts_.token_ttl_seconds; }

private:
    struct UserEntry {
        std::string salt; // hex
        std::string hash; // sha256(salt + ":" + password) hex
    };
    struct Session {
        std::string user;
        int64_t expires_at = 0; // epoch seconds
    };

    static std::string HashPassword(const std::string& salt, const std::string& password);
    void PersistLocked();

    Options opts_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, UserEntry> users_;
    std::unordered_map<std::string, Session> sessions_;
};

} // namespace pl::minisearch::auth
