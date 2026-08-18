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

#include "cpp/pl/minisearch/auth/keystore.h"

#include <butil/third_party/rapidjson/document.h>
#include <butil/third_party/rapidjson/stringbuffer.h>
#include <butil/third_party/rapidjson/writer.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sys/stat.h>

#include "cpp/pl/minisearch/core/schema.h"

namespace rj = BUTIL_RAPIDJSON_NAMESPACE;

namespace pl::minisearch::auth {

namespace {

std::string to_hex(const unsigned char* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

// 32 random bytes -> base62（256 bit 熵取 43 字符，模偏对 CSPRNG 输出可忽略）
std::string random_key() {
    static const char kBase62[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned char raw[32];
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        return {};
    }
    std::string out = "msk_";
    out.reserve(4 + 43);
    for (size_t i = 0; i < sizeof(raw); ++i) {
        out.push_back(kBase62[raw[i] % 62]);
    }
    return out;
}

int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

// key_id 形如 "k_<n>"，解析序号；无法解析返回 0。
int64_t key_id_seq(const std::string& key_id) {
    if (key_id.rfind("k_", 0) != 0) {
        return 0;
    }
    try {
        return std::stoll(key_id.substr(2));
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

std::string RoleToString(Role role) {
    switch (role) {
        case Role::kAdmin:
            return "admin";
        case Role::kTenantAdmin:
            return "tenant_admin";
        case Role::kWriter:
            return "writer";
        case Role::kReader:
            return "reader";
    }
    return "reader";
}

std::optional<Role> RoleFromString(const std::string& role) {
    if (role == "admin")
        return Role::kAdmin;
    if (role == "tenant_admin")
        return Role::kTenantAdmin;
    if (role == "writer")
        return Role::kWriter;
    if (role == "reader")
        return Role::kReader;
    return std::nullopt;
}

std::string KeyStore::HashKey(const std::string& key) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(key.data()), key.size(), digest);
    return to_hex(digest, sizeof(digest));
}

KeyStore::KeyStore(std::string path) : path_(std::move(path)) {
    if (path_.empty()) {
        return;
    }
    std::ifstream in(path_);
    if (!in.is_open()) {
        return;
    }
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    rj::Document doc;
    if (doc.Parse(content.c_str()).HasParseError() || !doc.IsObject() || !doc.HasMember("keys") ||
        !doc["keys"].IsArray()) {
        return; // 无法解析视为空 store（首次启动或文件损坏时人工介入）
    }
    const rj::Value& keys = doc["keys"];
    for (rj::SizeType i = 0; i < keys.Size(); ++i) {
        const rj::Value& item = keys[i];
        if (!item.IsObject() || !item.HasMember("hash") || !item.HasMember("key_id") ||
            !item.HasMember("tenant") || !item.HasMember("role")) {
            continue;
        }
        Entry entry;
        entry.hash = item["hash"].GetString();
        entry.key_id = item["key_id"].GetString();
        entry.principal.tenant = item["tenant"].GetString();
        entry.principal.role = RoleFromString(item["role"].GetString()).value_or(Role::kReader);
        entry.principal.key_id = entry.key_id;
        entry.created_at = item.HasMember("created_at") ? item["created_at"].GetInt64() : 0;
        entry.revoked = item.HasMember("revoked") && item["revoked"].IsBool()
                            ? item["revoked"].GetBool()
                            : false;
        if (item.HasMember("collections") && item["collections"].IsArray()) {
            const rj::Value& cols = item["collections"];
            for (rj::SizeType j = 0; j < cols.Size(); ++j) {
                if (cols[j].IsString()) {
                    entry.principal.collections.push_back(cols[j].GetString());
                }
            }
        }
        const size_t index = entries_.size();
        entries_.push_back(std::move(entry));
        by_hash_[entries_[index].hash] = index;
        by_id_[entries_[index].key_id] = index;
        next_id_ = std::max(next_id_, key_id_seq(entries_[index].key_id) + 1);
    }
}

KeyStore::Issued KeyStore::Issue(const std::string& tenant,
                                 Role role,
                                 std::vector<std::string> collections) {
    std::lock_guard<std::mutex> lock(mu_);
    Issued issued;
    if (!core::IsValidResourceName(tenant)) {
        return issued; // 非法租户名：拒绝签发
    }
    for (const auto& c : collections) {
        if (!core::IsValidResourceName(c)) {
            return issued;
        }
    }
    issued.key = random_key();
    if (issued.key.empty()) {
        return {};
    }
    issued.key_id = "k_" + std::to_string(next_id_++);

    Entry entry;
    entry.key_id = issued.key_id;
    entry.hash = HashKey(issued.key);
    entry.principal.tenant = tenant;
    entry.principal.role = role;
    entry.principal.collections = std::move(collections);
    entry.principal.key_id = issued.key_id;
    entry.created_at = now_seconds();

    const size_t index = entries_.size();
    by_hash_[entry.hash] = index;
    by_id_[issued.key_id] = index;
    entries_.push_back(std::move(entry));
    PersistLocked();
    return issued;
}

std::optional<Principal> KeyStore::Authenticate(const std::string& bearer) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = by_hash_.find(HashKey(bearer));
    if (it == by_hash_.end() || entries_[it->second].revoked) {
        return std::nullopt;
    }
    return entries_[it->second].principal;
}

bool KeyStore::Revoke(const std::string& key_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto id_it = by_id_.find(key_id);
    if (id_it == by_id_.end() || entries_[id_it->second].revoked) {
        return false;
    }
    entries_[id_it->second].revoked = true;
    PersistLocked();
    return true;
}

bool KeyStore::MoveKey(const std::string& key_id, const std::string& dst_tenant) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!core::IsValidResourceName(dst_tenant)) {
        return false;
    }
    auto id_it = by_id_.find(key_id);
    if (id_it == by_id_.end() || entries_[id_it->second].revoked) {
        return false;
    }
    entries_[id_it->second].principal.tenant = dst_tenant;
    PersistLocked();
    return true;
}

std::vector<KeyStore::Entry> KeyStore::List() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_;
}

size_t KeyStore::Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

std::optional<KeyStore::Issued> KeyStore::BootstrapIfNeeded(const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!entries_.empty()) {
        return std::nullopt;
    }
    Issued issued;
    issued.key = random_key();
    if (issued.key.empty()) {
        return std::nullopt;
    }
    issued.key_id = "k_" + std::to_string(next_id_++);

    Entry entry;
    entry.key_id = issued.key_id;
    entry.hash = HashKey(issued.key);
    entry.principal.tenant = "default";
    entry.principal.role = Role::kAdmin;
    entry.principal.key_id = issued.key_id;
    entry.created_at = now_seconds();
    const size_t index = entries_.size();
    by_hash_[entry.hash] = index;
    by_id_[issued.key_id] = index;
    entries_.push_back(std::move(entry));
    PersistLocked();

    const std::string bootstrap = data_dir + "/bootstrap.key";
    write_file(bootstrap, issued.key + "\n");
    ::chmod(bootstrap.c_str(), 0600);
    return issued;
}

void KeyStore::PersistLocked() {
    if (path_.empty()) {
        return;
    }
    rj::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    rj::Value keys(rj::kArrayType);
    for (const auto& entry : entries_) {
        rj::Value item(rj::kObjectType);
        item.AddMember("hash", rj::Value(entry.hash.c_str(), alloc), alloc);
        item.AddMember("key_id", rj::Value(entry.key_id.c_str(), alloc), alloc);
        item.AddMember("tenant", rj::Value(entry.principal.tenant.c_str(), alloc), alloc);
        item.AddMember("role", rj::Value(RoleToString(entry.principal.role).c_str(), alloc), alloc);
        rj::Value collections(rj::kArrayType);
        for (const auto& c : entry.principal.collections) {
            collections.PushBack(rj::Value(c.c_str(), alloc), alloc);
        }
        item.AddMember("collections", collections, alloc);
        item.AddMember("created_at", entry.created_at, alloc);
        item.AddMember("revoked", entry.revoked, alloc);
        keys.PushBack(item, alloc);
    }
    doc.AddMember("keys", keys, alloc);
    rj::StringBuffer buf;
    rj::Writer<rj::StringBuffer> writer(buf);
    doc.Accept(writer);

    const std::string tmp = path_ + ".tmp";
    if (!write_file(tmp, buf.GetString()) || std::rename(tmp.c_str(), path_.c_str()) != 0) {
        // 持久化失败仅影响重启后状态；内存索引仍有效
    }
}

} // namespace pl::minisearch::auth
