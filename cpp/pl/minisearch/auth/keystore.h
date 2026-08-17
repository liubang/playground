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
#include <vector>

namespace pl::minisearch::auth {

enum class Role {
    kAdmin,
    kTenantAdmin,
    kWriter,
    kReader,
};

std::string RoleToString(Role role);
std::optional<Role> RoleFromString(const std::string& role);

// Authenticated request context (DESIGN.md §10). `collections` is a
// whitelist; empty means every collection in the tenant.
struct Principal {
    std::string tenant;
    Role role = Role::kReader;
    std::vector<std::string> collections;
    std::string key_id;
};

// API key store: keys are `msk_<base62>`; only sha256(key) is persisted.
// File format (<data_dir>/auth/keys.json, tmp+rename atomic write):
//   {"keys": [{"hash", "key_id", "tenant", "role",
//              "collections": [...], "created_at", "revoked"}, ...]}
// Revocation is a soft delete (revoked=true persisted) and is effective
// immediately for authentication.
class KeyStore {
public:
    struct Issued {
        std::string key_id;
        std::string key; // plaintext, returned exactly once
    };

    // path 为空表示不持久化（纯内存，用于测试）。
    explicit KeyStore(std::string path);

    // 生成并登记一把新 key。tenant 必须是合法资源名
    // （core::IsValidResourceName），否则返回空 key。
    Issued Issue(const std::string& tenant, Role role, std::vector<std::string> collections);

    // Bearer token（不含 "Bearer " 前缀）→ Principal；未知/已吊销返回 nullopt。
    std::optional<Principal> Authenticate(const std::string& bearer) const;

    // 软删除：置 revoked 并落盘。不存在或已吊销返回 false。
    bool Revoke(const std::string& key_id);

    struct Entry {
        std::string key_id;
        std::string hash; // sha256(key)，落盘即此值
        Principal principal;
        int64_t created_at = 0; // epoch seconds
        bool revoked = false;
    };
    std::vector<Entry> List() const;

    size_t Size() const;

    // bootstrap（DESIGN.md §10.3）：store 为空时签发一把 default 租户 admin key，
    // 明文写入 <data_dir>/bootstrap.key（0600）并返回；非空返回 nullopt。
    std::optional<Issued> BootstrapIfNeeded(const std::string& data_dir);

    static std::string HashKey(const std::string& key);

private:
    void PersistLocked();

    std::string path_;
    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, size_t> by_hash_; // sha256(key) -> index
    std::unordered_map<std::string, size_t> by_id_;   // key_id -> index
    int64_t next_id_ = 1;
};

} // namespace pl::minisearch::auth
